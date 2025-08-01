// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <core/common/inlined_containers.h>
#include "tensor_allocator.h"
#include "mem_pattern.h"
#include "ort_value_pattern_planner.h"
#include "utils.h"
#include "tensorprotoutils.h"
#include "bfc_arena.h"

namespace onnxruntime {

class TensorAllocatorWithMemPattern : public ITensorAllocator {
 private:
  OrtValuePatternPlanner planner_;
  MemoryPatternGroup mem_patterns_;
  InlinedVector<BufferUniquePtr>& weights_buffers_;
  InlinedHashMap<OrtDevice, void*> buffers_;
  bool is_sealed_ = false;
  const ExecutionPlanBase& seq_plan_;

  common::Status AllocatePlannedBuffersAndReportTotalSize(
      InlinedHashMap<OrtDevice, size_t>& planned_memory_sizes_in_byte) {
    const size_t location_len = mem_patterns_.locations.size();
    planned_memory_sizes_in_byte.reserve(location_len);
    for (size_t i = 0; i < location_len; ++i) {
      auto& location = mem_patterns_.locations[i];
      auto alloc = GetInitializerAllocator(location);
      if (!alloc)
        return Status(common::ONNXRUNTIME, common::FAIL,
                      "Failed to get allocator for location: " + location.ToString());

      // Don't allocate memory when there is no memory usage.
      if (mem_patterns_.patterns[i].PeakSize() <= 0) {
        continue;
      }

      const auto peak_size = mem_patterns_.patterns[i].PeakSize();
      // use Reserve for initializers so they don't affect arena growth patterns if an arena is involved.
      void* buffer = alloc->Reserve(peak_size);

      auto buffer_ptr = BufferUniquePtr(buffer, BufferDeleter(std::move(alloc)));
      auto kvp = buffers_.insert(std::make_pair(location, buffer));
      if (!kvp.second) {
        return Status(common::ONNXRUNTIME, common::FAIL, "duplicated location");
      }
      weights_buffers_.push_back(std::move(buffer_ptr));

      planned_memory_sizes_in_byte[location] += peak_size;
    }
    return Status::OK();
  }

 public:
  TensorAllocatorWithMemPattern(const ExecutionPlanBase& execution_plan, const SessionState& session_state,
                                InlinedVector<BufferUniquePtr>& weights_buffers)
      : ITensorAllocator(session_state),
        planner_(execution_plan, /*using counters*/ false),
        weights_buffers_(weights_buffers),
        seq_plan_(execution_plan) {}

  common::Status FinalizePlan(InlinedHashMap<OrtDevice, size_t>& planned_memory_sizes_in_byte) override {
    ORT_RETURN_IF_ERROR(planner_.GeneratePatterns(mem_patterns_));
    ORT_RETURN_IF_ERROR(AllocatePlannedBuffersAndReportTotalSize(planned_memory_sizes_in_byte));
    is_sealed_ = true;
    return Status::OK();
  }

  common::Status GetPreallocatedBuffer(int ort_value_index, const std::string& name,
                                       std::optional<MemBuffer>& buf_out, AllocatorPtr& alloc_out) override {
    if (!is_sealed_) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Internal error.");
    }

    const struct OrtDevice& location = seq_plan_.GetLocation(ort_value_index);
    auto pattern = mem_patterns_.GetPatterns(location);
    if (pattern == nullptr) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Mem pattern for initializer ", name, " is not found");
    }

    // if block is not found, means this ort_value is not traced
    // fall back to allocate separate buffer.
    // if it->second.get() is null, then fall back to the block not found case
    auto block = pattern->GetBlock(ort_value_index);
    if (nullptr == block) {
      // not traced, only return allocator
      alloc_out = GetInitializerAllocator(location);
      return Status::OK();
    }

    auto it = buffers_.find(location);
    if (it == buffers_.end()) {
      if (block != nullptr && block->size_ == 0) {
        // Because the size is 0, this miss find is expected. we won't allocate a buffer with size of zero.
        buf_out.emplace(nullptr, 0, GetInitializerAllocator(location)->Info());
        return Status::OK();
      }
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Weight buffer for initializer '", name, "' is not found");
    }

    if (block == nullptr || it->second == nullptr) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Get preallocated buffer for initializer '", name, "' failed");
    }

    buf_out.emplace(static_cast<char*>(it->second) + block->offset_, block->size_,
                    GetInitializerAllocator(location)->Info());
    return Status::OK();
  }
  common::Status Trace(int id, const ONNX_NAMESPACE::TensorProto* value) override {
    if (is_sealed_) {
      return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Internal error.");
    }
    size_t len = 0;
    ORT_RETURN_IF_ERROR(utils::GetSizeInBytesFromTensorProto<kAllocAlignment>(*value, &len));
    ORT_RETURN_IF_ERROR(planner_.TraceAllocation(id, len));
    return Status::OK();
  }

  const MemoryPatternGroup& GetMemPatterns() override {
    return mem_patterns_;
  }
};
}  // namespace onnxruntime
