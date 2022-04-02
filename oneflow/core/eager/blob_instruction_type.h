/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#ifndef ONEFLOW_CORE_EAGER_BLOB_INSTRUCTION_TYPE_H_
#define ONEFLOW_CORE_EAGER_BLOB_INSTRUCTION_TYPE_H_

#include "oneflow/core/intrusive/flat_msg_view.h"
#include "oneflow/core/vm/instruction_type.h"
#include "oneflow/core/common/stream_role.h"
#include "oneflow/core/common/singleton_ptr.h"
#include "oneflow/core/vm/cuda_optional_event_record_status_querier.h"
#include "oneflow/core/vm/ep_optional_event_record_status_querier.h"
#include "oneflow/core/vm/stream.h"
#include "oneflow/core/device/cuda_event.h"
#include "oneflow/core/vm/ep_event.h"
#include "oneflow/core/vm/ep_device_context.h"
#include "oneflow/core/common/env_var/ep_based_cuda.h"

namespace oneflow {
namespace vm {

class TensorViewInstructionType final : public vm::InstructionType {
 public:
  TensorViewInstructionType() = default;
  ~TensorViewInstructionType() override = default;

  std::string DebugName(const vm::Instruction& instruction) const override { return "TensorView"; }
  void Compute(vm::Instruction* instruction) const override;
};

class AccessBlobByCallbackInstructionType final : public vm::InstructionType {
 public:
  AccessBlobByCallbackInstructionType() = default;
  ~AccessBlobByCallbackInstructionType() override = default;

  std::string DebugName(const vm::Instruction& instruction) const override {
    return "AccessBlobByCallback";
  }
  void Compute(vm::Instruction* instruction) const override;
};

class CpuRecordEventInstructionType final : public vm::InstructionType {
 public:
  CpuRecordEventInstructionType() = default;
  ~CpuRecordEventInstructionType() override = default;

  std::string DebugName(const vm::Instruction& instruction) const override { return "RecordEvent"; }
  void Compute(vm::Instruction* instruction) const override {}
};

#ifdef WITH_CUDA

class CudaRecordEventInstructionType final : public vm::InstructionType {
 public:
  CudaRecordEventInstructionType() = default;
  ~CudaRecordEventInstructionType() override = default;

  InstructionFuseType fuse_type() const override { return kEnableInstructionFuseAsTailOnly; }

  void InitInstructionStatus(Instruction* instruction) const override {
    auto* status_buffer = instruction->mut_status_buffer();
    auto* stream = instruction->mut_stream();
    instruction->stream_type().InitInstructionStatus(*stream, status_buffer);
    auto* event_provider = dynamic_cast<QueryCudaEventProvider*>(stream->device_ctx().get());
    const auto& cuda_event = CHECK_NOTNULL(event_provider)->GetCudaEvent();
    auto* data_ptr = status_buffer->mut_buffer();
    CudaOptionalEventRecordStatusQuerier::MutCast(data_ptr)->reset_cuda_event(cuda_event);
  }
  std::string DebugName(const vm::Instruction& instruction) const override { return "RecordEvent"; }
  void Compute(vm::Instruction* instruction) const override {}
};

#endif

class EpRecordEventInstructionType final : public vm::InstructionType {
 public:
  EpRecordEventInstructionType() = default;
  ~EpRecordEventInstructionType() override = default;

  InstructionFuseType fuse_type() const override { return kEnableInstructionFuseAsTailOnly; }

  void InitInstructionStatus(Instruction* instruction) const override {
    auto* status_buffer = instruction->mut_status_buffer();
    auto* stream = instruction->mut_stream();
    instruction->stream_type().InitInstructionStatus(*stream, status_buffer);
    auto* ep_device_ctx = static_cast<EpDeviceCtx*>(stream->device_ctx().get());
    auto* ep_event_provider = ep_device_ctx->ep_event_provider();
    const auto& ep_event = CHECK_NOTNULL(ep_event_provider)->GetReusedEpEvent();
    auto* data_ptr = status_buffer->mut_buffer();
    EpOptionalEventRecordStatusQuerier::MutCast(data_ptr)->reset_ep_event(ep_event);
  }
  std::string DebugName(const vm::Instruction& instruction) const override { return "RecordEvent"; }
  void Compute(vm::Instruction* instruction) const override {}
};
}  // namespace vm

struct GetRecordEventInstructionType {
  static Maybe<const vm::InstructionType*> Case(StreamRoleCase<StreamRole::kInvalid>,
                                                DeviceType device_type) {  // NOLINT
    UNIMPLEMENTED_THEN_RETURN();
  }
  static Maybe<const vm::InstructionType*> Case(StreamRoleCase<StreamRole::kCompute>,
                                                DeviceType device_type) {
    return GetInstructionType(device_type);
  }
  static Maybe<const vm::InstructionType*> Case(StreamRoleCase<StreamRole::kHost2Device>,
                                                DeviceType device_type) {
    return GetInstructionType(device_type);
  }
  static Maybe<const vm::InstructionType*> Case(StreamRoleCase<StreamRole::kDevice2Host>,
                                                DeviceType device_type) {
    return GetInstructionType(device_type);
  }
  static Maybe<const vm::InstructionType*> Case(StreamRoleCase<StreamRole::kSyncedLaunchedCommNet>,
                                                DeviceType device_type) {
    return GetInstructionType(device_type);
  }
  static Maybe<const vm::InstructionType*> Case(StreamRoleCase<StreamRole::kAsyncedLaunchedCommNet>,
                                                DeviceType device_type) {
    return GetInstructionType(device_type);
  }
  static Maybe<const vm::InstructionType*> Case(StreamRoleCase<StreamRole::kBarrier>,
                                                DeviceType device_type) {
    UNIMPLEMENTED_THEN_RETURN();
  }
  static Maybe<const vm::InstructionType*> Case(StreamRoleCase<StreamRole::kCriticalSection>,
                                                DeviceType device_type) {
    UNIMPLEMENTED_THEN_RETURN();
  }
  static Maybe<const vm::InstructionType*> Case(StreamRoleCase<StreamRole::kLazyJobLauncher>,
                                                DeviceType device_type) {
    UNIMPLEMENTED_THEN_RETURN();
  }

 private:
  static Maybe<const vm::InstructionType*> GetInstructionType(DeviceType device_type) {
    if (device_type == DeviceType::kCPU) {
      return SingletonPtr<vm::CpuRecordEventInstructionType>();
    } else if (device_type == DeviceType::kCUDA) {
      if (ThreadLocalEnvBool<ONEFLOW_EP_BASED_CUDA>()) {
        return SingletonPtr<vm::EpRecordEventInstructionType>();
      } else {
#ifdef WITH_CUDA
        return SingletonPtr<vm::CudaRecordEventInstructionType>();
#else
        UNIMPLEMENTED_THEN_RETURN();
#endif
      }
    } else {
      return SingletonPtr<vm::EpRecordEventInstructionType>();
    }
  }
};

}  // namespace oneflow
#endif  // ONEFLOW_CORE_EAGER_BLOB_INSTRUCTION_TYPE_H_
