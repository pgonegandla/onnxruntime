// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License

#pragma once

#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/providers/qnn/ort_api.h"
#include "core/providers/qnn/builder/qnn_backend_manager.h"
#include "core/providers/qnn/builder/qnn_model.h"
#include "core/providers/qnn/builder/qnn_configs_helper.h"
#include "core/providers/qnn/rpcmem_library.h"
#include "HTP/QnnHtpGraph.h"

namespace onnxruntime {

// Logical device representation.
class QNNExecutionProvider : public IExecutionProvider {
 public:
  explicit QNNExecutionProvider(const ProviderOptions& provider_options_map, const ConfigOptions* config_options);
  virtual ~QNNExecutionProvider();
  ORT_DISALLOW_COPY_ASSIGNMENT_AND_MOVE(QNNExecutionProvider);

  // we implement the Compile that takes FusedNodeAndGraph instances
  FusionStyle GetFusionStyle() const override { return FusionStyle::FilteredGraphViewer; }

  std::vector<std::unique_ptr<ComputeCapability>>
  GetCapability(const onnxruntime::GraphViewer& graph_view,
                const IKernelLookup& /*kernel_lookup*/,
                const GraphOptimizerRegistry& /* graph_optimizer_registry */,
                IResourceAccountant* /* resource_accountant */) const override;

  Status Compile(const std::vector<FusedNodeAndGraph>& fused_nodes_and_graphs,
                 std::vector<NodeComputeInfo>& node_compute_funcs) override;

  const void* GetExecutionHandle() const noexcept override {
    return nullptr;
  }

  DataLayout GetPreferredLayout() const override;

  std::optional<bool> ShouldConvertDataLayoutForOp(std::string_view node_domain,
                                                   std::string_view node_op_type,
                                                   DataLayout target_data_layout) const override;

  const InlinedVector<const Node*> GetEpContextNodes() const override;

  Status OnRunStart(const onnxruntime::RunOptions& run_options) override;

  Status OnRunEnd(bool sync_stream, const onnxruntime::RunOptions& run_options) override;

  std::vector<AllocatorPtr> CreatePreferredAllocators() override;

  OrtDevice GetOrtDeviceByMemType(OrtMemType mem_type) const override;

  Status SetEpDynamicOptions(gsl::span<const char* const> keys,
                             gsl::span<const char* const> value) override;

 private:
  std::unordered_set<const Node*> GetSupportedNodes(const GraphViewer& graph_viewer,
                                                    const std::unordered_map<const Node*, const NodeUnit*>& node_unit_map,
                                                    const size_t node_unit_size,
                                                    const logging::Logger& logger) const;

  Status CreateComputeFunc(std::vector<NodeComputeInfo>& node_compute_funcs,
                           const logging::Logger& logger);

  Status CompileFromOrtGraph(const std::vector<FusedNodeAndGraph>& fused_nodes_and_graphs,
                             std::vector<NodeComputeInfo>& node_compute_funcs,
                             const logging::Logger& logger);

  void ParseHtpGraphFinalizationOptimizationMode(const std::string& htp_graph_finalization_opt_mode_string);

  void InitQnnHtpGraphConfigs(qnn::QnnConfigsBuilder<QnnGraph_Config_t, QnnHtpGraph_CustomConfig_t>& configs_builder) const;

  qnn::ProfilingLevel GetProfilingLevelFromETWLevel(unsigned char level);

  bool IsHtpSharedMemoryAllocatorAvailable() const { return rpcmem_library_ != nullptr; }

 private:
  qnn::HtpGraphFinalizationOptimizationMode htp_graph_finalization_opt_mode_ = qnn::HtpGraphFinalizationOptimizationMode::kDefault;
  // Note: Using shared_ptr<QnnBackendManager> so that we can refer to it with a weak_ptr from a
  // HtpSharedMemoryAllocator allocation cleanup callback.
  std::shared_ptr<qnn::QnnBackendManager> qnn_backend_manager_;
  std::unordered_map<std::string, std::unique_ptr<qnn::QnnModel>> qnn_models_;
  bool context_cache_enabled_ = false;
  std::string context_cache_path_cfg_ = "";
  std::string context_node_name_prefix_ = "";
  bool disable_cpu_ep_fallback_ = false;  // True if CPU EP fallback has been disabled for this session.
  bool qnn_context_embed_mode_ = true;
  int32_t vtcm_size_in_mb_ = 0;
  bool enable_vtcm_backup_buffer_sharing_ = false;
  std::unique_ptr<onnxruntime::Model> qnn_ep_context_model_;
  std::unique_ptr<ModelMetadefIdGenerator> metadef_id_generator_;
  uint32_t device_id_ = 0;
  qnn::HtpPerformanceMode default_htp_performance_mode_ = qnn::HtpPerformanceMode::kHtpDefault;
  uint32_t default_rpc_control_latency_ = 0;
  uint32_t default_rpc_polling_time_ = 0;
  bool enable_HTP_FP16_precision_ = true;
  bool share_ep_contexts_ = false;
  bool stop_share_ep_contexts_ = false;
  bool enable_spill_fill_buffer_ = false;
#if defined(_WIN32)
  onnxruntime::logging::EtwRegistrationManager::EtwInternalCallback callback_ETWSink_provider_ = nullptr;
#endif
  qnn::ModelSettings model_settings_ = {};
  bool dump_json_qnn_graph_ = false;
  std::string json_qnn_graph_dir_ = "";

  // Whether this is set depends on a session option enabling it and if the RPCMEM dynamic library is available.
  // This is potentially shared with HtpSharedMemoryAllocator which may be returned by CreatePreferredAllocators().
  std::shared_ptr<qnn::RpcMemLibrary> rpcmem_library_ = nullptr;

  class PerThreadContext final {
   public:
    PerThreadContext(qnn::QnnBackendManager* qnn_backend_manager,
                     uint32_t device_id, uint32_t core_id,
                     qnn::HtpPerformanceMode default_htp_performance_mode,
                     uint32_t default_rpc_control_latency,
                     uint32_t default_rpc_polling_time);
    ~PerThreadContext();
    ORT_DISALLOW_COPY_ASSIGNMENT_AND_MOVE(PerThreadContext);

    bool IsHtpPowerConfigIdValid() { return is_htp_power_config_id_valid_; }

    uint32_t GetHtpPowerConfigId() { return htp_power_config_id_; }

   private:
    bool is_htp_power_config_id_valid_ = false;
    uint32_t htp_power_config_id_ = 0;
    qnn::QnnBackendManager* qnn_backend_manager_;
  };

  using PerThreadContextMap = std::unordered_map<const QNNExecutionProvider*, std::weak_ptr<PerThreadContext>>;

  struct ContextCacheHolder {
    ContextCacheHolder() {
      RunOnUnload([&, weak_p_ = std::weak_ptr<PerThreadContextMap>(p)] {
        if (auto lock = weak_p_.lock())
          p.reset();
      });
    }

    std::shared_ptr<PerThreadContextMap> p = std::make_shared<PerThreadContextMap>();
  };

  static const std::shared_ptr<PerThreadContextMap>& PerThreadContextCache() {
    thread_local const ContextCacheHolder per_thread_context_cache;
    return per_thread_context_cache.p;
  }

  struct PerThreadContextState {
    // contexts that are currently active
    std::set<std::shared_ptr<PerThreadContext>, std::owner_less<std::shared_ptr<PerThreadContext>>> active_contexts;
    // contexts available for reuse
    std::vector<std::shared_ptr<PerThreadContext>> retired_context_pool;
    // weak references to thread local caches from which this QNNExecutionProvider instance's entry should be removed
    // upon destruction
    std::set<std::weak_ptr<PerThreadContextMap>, std::owner_less<std::weak_ptr<PerThreadContextMap>>>
        caches_to_update_on_destruction;
    // synchronizes access to PerThreadContextState members
    std::mutex mutex;
  };

  // The execution provider maintains the PerThreadContexts in this structure.
  // Synchronization is required to update the contained structures.
  // On the other hand, access to an individual PerThreadContext is assumed to be from a single thread at a time,
  // so synchronization is not required for that.
  mutable PerThreadContextState context_state_;

  PerThreadContext& GetPerThreadContext() const;
  void ReleasePerThreadContext() const;
};

}  // namespace onnxruntime
