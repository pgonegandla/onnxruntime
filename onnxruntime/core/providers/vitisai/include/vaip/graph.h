// Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.
// Licensed under the MIT License.
#pragma once
#include "./node.h"
#include "vaip/my_ort.h"
#include <gsl/gsl>
#include <functional>
namespace vaip {
using namespace onnxruntime;

void graph_remove_node(Graph& graph, const NodeInput& node_input);
Node& graph_add_node(Graph& graph, const std::string& name, const std::string& op_type, const std::string& description,
                     const std::vector<const NodeArg*>& input_args, const std::vector<const NodeArg*>& output_args,
                     const NodeAttributes& attributes, const std::string& domain);
void graph_save(const Graph& graph, const std::string& filename, const std::string& dat_filename,
                size_t initializer_size_threshold);
vaip_core::DllSafe<std::string> graph_save_string(const Graph& graph);
Node& graph_fuse(Graph& graph, const std::string& name, const std::string& op_type, const std::vector<size_t>& nodes,
                 const std::vector<std::string>& inputs, const std::vector<std::string>& outputs,
                 const std::vector<std::string>& constant_initializers);
Model* model_clone(const Model& original_model, int64_t external_data_threshold);

void graph_reverse_dfs_from(
    const Graph& graph, gsl::span<const Node* const> from,
    const std::function<bool(const Node*)>& enter,
    const std::function<bool(const Node*)>& leave,
    const std::function<bool(const Node*, const Node*)>& comp,
    const std::function<bool(const Node* from, const Node* to)>&
        stop);
}  // namespace vaip
