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
#ifndef ONEFLOW_CORE_EAGER_CRITICAL_SECTION_PHY_INSTR_OPERAND_H_
#define ONEFLOW_CORE_EAGER_CRITICAL_SECTION_PHY_INSTR_OPERAND_H_

#include "oneflow/core/vm/instruction_operand.msg.h"
#include "oneflow/core/eager/eager_blob_object.h"
#include "oneflow/core/device/event_record.h"
#include "oneflow/core/framework/nn_graph_if.h"
#include "oneflow/core/common/buffer_manager.h"

namespace oneflow {

namespace one {

using EagerBlobObjectListPtr =
    std::shared_ptr<const std::vector<std::shared_ptr<vm::EagerBlobObject>>>;

}

namespace vm {

class CriticalSectionBeginPhyInstrOperand : public PhyInstrOperand {
 public:
  CriticalSectionBeginPhyInstrOperand(const CriticalSectionBeginPhyInstrOperand&) = delete;
  CriticalSectionBeginPhyInstrOperand(CriticalSectionBeginPhyInstrOperand&&) = delete;
  CriticalSectionBeginPhyInstrOperand& operator=(const CriticalSectionBeginPhyInstrOperand&) =
      delete;
  CriticalSectionBeginPhyInstrOperand& operator=(CriticalSectionBeginPhyInstrOperand&&) = delete;
  virtual ~CriticalSectionBeginPhyInstrOperand() = default;

  explicit CriticalSectionBeginPhyInstrOperand(
      const std::shared_ptr<NNGraphIf>& nn_graph,
      const one::EagerBlobObjectListPtr& eager_blob_objects,
      const std::shared_ptr<HashMap<std::string, std::shared_ptr<SharedEventRecord>>>& op_name2end_event_record)
      : nn_graph_(nn_graph), eager_blob_objects_(eager_blob_objects), op_name2end_event_record_(op_name2end_event_record) {}

  const one::EagerBlobObjectListPtr& eager_blob_objects() const { return eager_blob_objects_; }
  const std::shared_ptr<HashMap<std::string, std::shared_ptr<SharedEventRecord>>>& op_name2end_event_recor() const { return op_name2end_event_record_; }

  void ForEachMirroredObject(
      const std::function<void(vm::MirroredObject* infer, vm::MirroredObject* compute)>&) const;

  void ForEachMutMirroredObject(
      const std::function<void(vm::MirroredObject* infer, vm::MirroredObject* compute)>&)
      const override;

  virtual const std::vector<std::string>& interface_op_names() const = 0;
  virtual const std::vector<bool>& interfaces_valid() const = 0;
  virtual std::string GetInterfaceBufferName(const std::string& job_name, const std::string& op_name) const = 0;
  virtual std::string GetInterfaceCriticalSectionCallbackBufferName(const std::string& job_name) const = 0;
  virtual std::string GetInterfaceCriticalSectionWaitBufferName(const std::string& job_name) const = 0;
  virtual void AccessBlobByCallback(int64_t of_blob_ptr, const std::string& op_name);

  void FinishInvalidInterfaceEventRecords();
  void Finish();

 protected:
  std::shared_ptr<NNGraphIf>& nn_graph_;
  one::EagerBlobObjectListPtr eager_blob_objects_;
  std::shared_ptr<HashMap<std::string, std::shared_ptr<SharedEventRecord>>> op_name2end_event_record_;
  HashMap<std::string, size_t> op_name2inferface_index_;
};

class InputCriticalSectionBeginPhyInstrOperand final : public CriticalSectionBeginPhyInstrOperand {
 public:
  InputCriticalSectionBeginPhyInstrOperand(
      const std::shared_ptr<NNGraphIf>& nn_graph,
      const one::EagerBlobObjectListPtr& eager_blob_objects,
      const std::shared_ptr<HashMap<std::string, std::shared_ptr<SharedEventRecord>>>& op_name2end_event_record)
      : CriticalSectionBeginPhyInstrOperand(nn_graph, eager_blob_objects, op_name2end_event_record) {
    CHECK_EQ(nn_graph->input_op_names().size(), eager_blob_objects->size());
    CHECK_EQ(nn_graph->input_op_names().size(), nn_graph->inputs_valid().size());
    for (int i = 0; i < nn_graph->input_op_names().size(); ++i) {
      CHECK(op_name2inferface_index_.emplace(nn_graph->input_op_names().at(i), i).second);
    }
  }

  ~InputCriticalSectionBeginPhyInstrOperand() override = default;

  // for inputs
  void ForEachConstMirroredObject(
      const std::function<void(vm::MirroredObject* infer, vm::MirroredObject* compute)>& DoEach)
      const override {
    ForEachMirroredObject(DoEach);
  }

  // for outputs
  void ForEachMut2MirroredObject(
      const std::function<void(vm::MirroredObject* infer, vm::MirroredObject* compute)>&)
      const override {}

  const std::vector<std::string>& interface_op_names() const override {
    return nn_graph_->input_op_names();
  }
  const std::vector<bool>& interfaces_valid() const override {
    return nn_graph_->inputs_valid();
  }
  std::string GetInterfaceBufferName(const std::string& job_name, const std::string& op_name) const override {
    return GetInputBufferName(job_name, op_name);
  }
  std::string GetInterfaceCriticalSectionCallbackBufferName(const std::string& job_name) const override {
    return GetInputCriticalSectionCallbackBufferName(job_name);
  }
  std::string GetInterfaceCriticalSectionWaitBufferName(const std::string& job_name) const override {
    return GetInputCriticalSectionWaitBufferName(job_name);
  }
  void AccessBlobByCallback(int64_t of_blob_ptr, const std::string& op_name) override;

};

class OutputCriticalSectionBeginPhyInstrOperand final : public CriticalSectionBeginPhyInstrOperand {
 public:
  OutputCriticalSectionBeginPhyInstrOperand(
      const std::shared_ptr<NNGraphIf>& nn_graph,
      const one::EagerBlobObjectListPtr& eager_blob_objects,
      const std::shared_ptr<HashMap<std::string, std::shared_ptr<SharedEventRecord>>>& op_name2end_event_record)
      : CriticalSectionBeginPhyInstrOperand(nn_graph, eager_blob_objects, op_name2end_event_record) {
    CHECK_EQ(nn_graph->output_op_names().size(), eager_blob_objects->size());
    CHECK_EQ(nn_graph->output_op_names().size(), nn_graph->outputs_valid().size());
    for (int i = 0; i < nn_graph->output_op_names().size(); ++i) {
      CHECK(op_name2inferface_index_.emplace(nn_graph->output_op_names().at(i), i).second);
    }
  }

  ~OutputCriticalSectionBeginPhyInstrOperand() override = default;

  // for inputs
  void ForEachConstMirroredObject(
      const std::function<void(vm::MirroredObject* infer, vm::MirroredObject* compute)>&)
      const override {}

  // for outputs
  void ForEachMut2MirroredObject(
      const std::function<void(vm::MirroredObject* infer, vm::MirroredObject* compute)>& DoEach)
      const override {
    ForEachMirroredObject(DoEach);
  }

  const std::vector<std::string>& interface_op_names() const override {
    return nn_graph_->output_op_names();
  }
  const std::vector<bool>& interfaces_valid() const override {
    return nn_graph_->outputs_valid();
  }
  std::string GetInterfaceBufferName(const std::string& job_name, const std::string& op_name) const override {
    return GetOutputBufferName(job_name, op_name);
  }
  std::string GetInterfaceCriticalSectionCallbackBufferName(const std::string& job_name) const override {
    return GetOutputCriticalSectionCallbackBufferName(job_name);
  }
  std::string GetInterfaceCriticalSectionWaitBufferName(const std::string& job_name) const override {
    return GetOutputCriticalSectionWaitBufferName(job_name);
  }
  void AccessBlobByCallback(int64_t of_blob_ptr, const std::string& op_name) override;

};

class CriticalSectionEndPhyInstrOperand : public PhyInstrOperand {
 public:
  CriticalSectionEndPhyInstrOperand(const std::shared_ptr<EagerBlobObject>& eager_blob_object,
                                    const std::shared_ptr<SharedEventRecord>& event_record)
      : eager_blob_object_(eager_blob_object), event_record_(event_record) {}
  virtual ~CriticalSectionEndPhyInstrOperand() = default;

  const std::shared_ptr<SharedEventRecord>& event_record() const { return event_record_; }

  void ForEachMirroredObject(
      const std::function<void(vm::MirroredObject* infer, vm::MirroredObject* compute)>&) const;

  void ForEachMutMirroredObject(
      const std::function<void(vm::MirroredObject* infer, vm::MirroredObject* compute)>&)
      const override;

 private:
  std::shared_ptr<EagerBlobObject> eager_blob_object_;
  std::shared_ptr<SharedEventRecord> event_record_;
};

class InputCriticalSecondEndPhyInstrOperand final : public CriticalSectionEndPhyInstrOperand {
 public:
  using CriticalSectionEndPhyInstrOperand::CriticalSectionEndPhyInstrOperand;
  ~InputCriticalSecondEndPhyInstrOperand() override = default;

  void ForEachConstMirroredObject(
      const std::function<void(vm::MirroredObject* infer, vm::MirroredObject* compute)>& DoEach)
      const override {
    ForEachMirroredObject(DoEach);
  }

  void ForEachMut2MirroredObject(
      const std::function<void(vm::MirroredObject* infer, vm::MirroredObject* compute)>&)
      const override {}
};

class OutputCriticalSecondEndPhyInstrOperand final : public CriticalSectionEndPhyInstrOperand {
 public:
  using CriticalSectionEndPhyInstrOperand::CriticalSectionEndPhyInstrOperand;
  ~OutputCriticalSecondEndPhyInstrOperand() override = default;

  // for inputs
  void ForEachConstMirroredObject(
      const std::function<void(vm::MirroredObject* infer, vm::MirroredObject* compute)>&)
      const override {}

  // for outputs
  void ForEachMut2MirroredObject(
      const std::function<void(vm::MirroredObject* infer, vm::MirroredObject* compute)>& DoEach)
      const override {
    ForEachMirroredObject(DoEach);
  }
};

}  // namespace vm
}  // namespace oneflow

#endif  // ONEFLOW_CORE_EAGER_CRITICAL_SECTION_PHY_INSTR_OPERAND_H_
