// Copyright (C) 2018-2022 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

///////////////////////////////////////////////////////////////////////////////////////////////////
#include "reorder_inst.h"
#include "primitive_type_base.h"
#include "intel_gpu/runtime/error_handler.hpp"
#include "json_object.h"
#include "intel_gpu/primitives/convolution.hpp"
#include "intel_gpu/primitives/eltwise.hpp"

#include <algorithm>
#include <string>

namespace cldnn {

primitive_type_id reorder::type_id() {
    static primitive_type_base<reorder> instance;
    return &instance;
}

layout reorder_inst::calc_output_layout(reorder_node const& node, kernel_impl_params const& impl_param) {
    auto input_layout = impl_param.input_layouts[0];
    auto ifmt = input_layout.format;

    auto desc = impl_param.typed_desc<reorder>();
    auto odt = *desc->output_data_type;
    auto ofmt = desc->output_format;
    auto op = desc->output_padding;

    if (ofmt == format::any) {
        ofmt = ifmt;
    }

    if (ifmt.is_nv12()) {
        auto data_size = tensor{ input_layout.batch(), input_layout.feature() * 3,
                                 input_layout.spatial(0), input_layout.spatial(1) };
        if (ofmt != ifmt)
            return layout(odt, ofmt, data_size, op);

        CLDNN_ERROR_MESSAGE(desc->id, "No image_nv12 to image_nv12 reorder is supported");
    } else if (ofmt.is_winograd() && ifmt.is_winograd()) {
        if (ofmt == ifmt)
            return layout(odt, ofmt, input_layout.get_tensor(), op);

        CLDNN_ERROR_MESSAGE(desc->id, "Reordering between winograd weights and data formats is unsupported");
    } else if (ifmt == format::image_2d_rgba) {
        return layout(data_types::f16, format::bfyx, input_layout.get_tensor(), op);
    }

    // transformation of data from standard to winograd
    if (ofmt == format::winograd_2x3_s1_data) {
        // some constants which are defined by F(2,3) with stride 1 -- todo: think about generic way to calculate them
        // for any F(r,m) with stride s
        // NOTE: FOR THE FOLLOWING CONSTANTS 'OUTPUT' MEANS OUTPUT OF WINOGRAD CONV (in standard domain) AND 'INPUT'
        // MEANS INPUT FOR WINOGRAD CONV (in winograd domain), THEREFORE 'INPUT' ACTUALLY REFERS TO THE OUTPUT OF THIS
        // CONVERSION (which is later fed as input for winograd conv)
        constexpr tensor::value_type output_tile_width = 2;  // by definition of F(2,3)
        constexpr tensor::value_type filter_width = 3;       // by definition of F(2,3)
        constexpr tensor::value_type filter_stride =
            1;  // by definition of format::winograd_2x3_s1_data (our assumption)

        constexpr tensor::value_type input_tile_width =
            filter_width +
            (output_tile_width - 1) * filter_stride;  // input tile should be large enought to hold data for
                                                      // computations of output tile (for given filter size and stride)

        // how many tiles do we need to produce
        // each input tile produces one output tile so we can find no. of input tiles by calculating no. of output tiles
        // (which is equal to width of an output divided by output tile width)
        tensor::value_type conv_output_width =
            input_layout.spatial(0) - filter_width + 1;
        tensor::value_type input_tiles_count_x = conv_output_width / output_tile_width;
        tensor::value_type output_width = input_tiles_count_x * input_tile_width;
        tensor::value_type output_height = input_layout.spatial(1);

        tensor::value_type padd_x = 0;
        tensor::value_type padd_y = (8 - ((output_height - 2) % 8)) % 8;
        if (conv_output_width % output_tile_width != 0) {  // leftovers
            output_width += 3;  // one tile is 4 elements from which only 3 first are used to generate first output
                                // value
            padd_x = 1;
        }

        auto data_size = tensor{input_layout.batch(), input_layout.feature(), output_width, output_height};
        tensor upper_padd = tensor{0, 0, padd_x, padd_y};
        return layout(odt, ofmt, data_size, padding{{0, 0, 0, 0}, upper_padd.sizes()});
    }

    // transformation of weights from standard to winograd
    if (ofmt == format::winograd_2x3_s1_weights || ofmt == format::winograd_2x3_s1_fused_weights) {
        CLDNN_ERROR_NOT_EQUAL(desc->id,
                              "input_layout.spatial(0)",
                              input_layout.spatial(0),
                              "expected value",
                              3,
                              "input for conversion to winograd_2x3_s1 weights format should have spatial size 3x3");
        CLDNN_ERROR_NOT_EQUAL(desc->id,
                              "input_layout.spatial(1)",
                              input_layout.spatial(1),
                              "expected value",
                              3,
                              "input for conversion to winograd_2x3_s1 weights format should have spatial size 3x3");

        return layout(odt, ofmt, tensor{input_layout.batch(), input_layout.feature(), 4, 3});
    } else if (ofmt == format::winograd_6x3_s1_fused_weights) {
        CLDNN_ERROR_NOT_EQUAL(desc->id,
                              "input_layout.spatial(0)",
                              input_layout.spatial(0),
                              "expected value",
                              3,
                              "input for conversion to winograd_2x3_s1 weights format should have spatial size 3x3");
        CLDNN_ERROR_NOT_EQUAL(desc->id,
                              "input_layout.spatial(1)",
                              input_layout.spatial(1),
                              "expected value",
                              3,
                              "input for conversion to winograd_2x3_s1 weights format should have spatial size 3x3");

        return layout(odt, ofmt, tensor{input_layout.batch(), input_layout.feature(), 8, 3});
    }

    // transformation of data from winograd to standard
    if (ifmt == format::winograd_2x3_s1_data) {
        constexpr tensor::value_type output_tile_width = 2;  // by definition of F(2,3)
        constexpr tensor::value_type filter_width = 3;       // by definition of F(2,3)
        constexpr tensor::value_type filter_stride =
            1;  // by definition of format::winograd_2x3_s1_data (our assumption)

        constexpr tensor::value_type input_tile_width =
            filter_width +
            (output_tile_width - 1) * filter_stride;  // input tile should be large enought to hold data for
                                                      // computations of output tile (for given filter size and stride)

        auto output_width = input_layout.spatial(0) / input_tile_width * output_tile_width;
        if (input_layout.spatial(0) % input_tile_width != 0)  // leftovers
            ++output_width;  // output tile is 2 by default, so we can have only 1 value as leftover

        return layout(odt,
                      ofmt,
                      tensor{input_layout.batch(),
                             input_layout.feature(),
                             output_width,
                             input_layout.spatial(1)});
    }

    // transformation of weights from winograd to standard
    if (ifmt == format::winograd_2x3_s1_weights || ifmt == format::winograd_2x3_s1_fused_weights ||
        ifmt == format::winograd_6x3_s1_fused_weights) {
        CLDNN_ERROR_MESSAGE(desc->id,
                            "Conversion of weights from winograd to standard domain is currently unsupported");
    }

    if (ofmt == format::bs_xs_xsv8_bsv8 || ofmt == format::os_i_osv8__ai8 || ofmt == format::os_i_osv16__ai8 || ofmt == format::bs_x_bsv16 ||
        ofmt == format::bfzyx || ifmt == format::bfzyx || ofmt == format::b_fs_zyx_fsv16 || ifmt == format::b_fs_zyx_fsv16 ||
        ofmt == format::bs_fs_zyx_bsv16_fsv16 || ifmt == format::bs_fs_zyx_bsv16_fsv16 ||
        ofmt == format::bs_fs_zyx_bsv16_fsv32 || ifmt == format::bs_fs_zyx_bsv16_fsv32 ||
        ofmt == format::b_fs_zyx_fsv32 || ifmt == format::b_fs_zyx_fsv32 ||
        ofmt == format::bs_fs_yx_bsv16_fsv16 || ifmt == format::bs_fs_yx_bsv16_fsv16) {
        return layout(odt, ofmt, input_layout.get_tensor().transform(ofmt, 1), op);
    } else if (ofmt != ifmt && (ofmt == format::bfwzyx || ifmt == format::bfwzyx)) {
        // TODO Shouldn't transform be called every time ifmt != ofmt?
        return layout(odt, ofmt, input_layout.get_tensor().transform(ofmt, 1), op);
    } else {
        return layout(odt, ofmt, input_layout.get_tensor(), op);
    }
}

std::string reorder_inst::to_string(reorder_node const& node) {
    auto desc = node.get_primitive();
    auto mean = desc->mean;
    auto node_info = node.desc_to_json();
    auto& input = node.input();

    std::stringstream primitive_description;

    json_composite reorder_info;
    reorder_info.add("input id", input.id());
    reorder_info.add("mean", mean);
    if (desc->subtract_per_feature.size() > 0) {
        reorder_info.add("subtract per feature", desc->subtract_per_feature);
    }

    node_info->add("reorder info", reorder_info);
    node_info->dump(primitive_description);

    return primitive_description.str();
}

reorder_inst::typed_primitive_inst(network& network, reorder_node const& node)
    : parent(network, node, !node.can_be_optimized()) {
    if (node.can_be_optimized())
        reuse_input();

    auto input_layout = node.input().get_output_layout();
    auto output_layout = node.get_output_layout();

    CLDNN_ERROR_LESS_THAN(node.id(),
                          "Input dimension size",
                          input_layout.get_tensor().raw.size(),
                          "ouput dimension size",
                          output_layout.get_tensor().raw.size(),
                          "Input dimension < output dimension. Reorder primitive woks only with same dimension sizes "
                          "(reorder) or when input > output (flatten).");

    if (!argument.subtract_per_feature.empty()) {
        CLDNN_ERROR_GREATER_THAN(node.id(),
                                 "Input feature dimension size",
                                 input_layout.get_tensor().feature.size(),
                                 "value",
                                 1,
                                 "Subtracting values work only for formats that have feature dimension == 1");
        if (input_layout.format != format::nv12) {
            CLDNN_ERROR_NOT_EQUAL(node.id(),
                "Input feature size[0]",
                static_cast<size_t>(input_layout.feature()),
                "argument subtract per feature size",
                argument.subtract_per_feature.size(),
                "Number of features/channels in input does not match the number of features/channels in "
                "values to subtract");
        }
    }
}

void reorder_inst::on_execute() {
    if (node.can_be_optimized())
        reuse_input();
}

void reorder_inst::reuse_input() {
    if (static_cast<bool>(_output) && _network.get_engine().is_the_same_buffer(output_memory(), input_memory()))
        return;

    build_deps();

    if (node.requires_reinterpret()) {
        _output = _network.get_engine().reinterpret_buffer(input_memory(), node.get_output_layout());
    } else {
        _output = input_memory_ptr();
    }
}

}  // namespace cldnn
