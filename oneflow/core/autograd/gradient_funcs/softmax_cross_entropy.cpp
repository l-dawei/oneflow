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
#include "oneflow/core/framework/op_expr_grad_function.h"
#include "oneflow/core/functional/functional.h"

namespace oneflow {
namespace one {

struct SoftmaxCrossEntropyGradState : public AutoGradCaptureState {
  bool requires_grad = false;
};

class SoftmaxCrossEntropy : public OpExprGradFunction<SoftmaxCrossEntropyGradState> {
 public:
  Maybe<void> Init(const OpExpr& op) override;
  Maybe<void> Capture(SoftmaxCrossEntropyGradState* ctx, const TensorTuple& inputs,
                      const TensorTuple& outputs, const AttrMap& attrs) const override;
  Maybe<void> Apply(const SoftmaxCrossEntropyGradState* ctx, const TensorTuple& out_grads,
                    TensorTuple* in_grads) const override;
};

Maybe<void> SoftmaxCrossEntropy::Init(const OpExpr& op) {
  const auto* fw_op_expr = dynamic_cast<const UserOpExpr*>(&op);
  CHECK_NOTNULL_OR_RETURN(fw_op_expr);
  return Maybe<void>::Ok();
}

Maybe<void> SoftmaxCrossEntropy::Capture(SoftmaxCrossEntropyGradState* ctx,
                                         const TensorTuple& inputs, const TensorTuple& outputs,
                                         const AttrMap& attrs) const {
  ctx->requires_grad = inputs.at(0)->requires_grad();
  if (!ctx->requires_grad) { return Maybe<void>::Ok(); }

  CHECK_EQ_OR_RETURN(inputs.size(), 2);
  CHECK_EQ_OR_RETURN(outputs.size(), 2);
  ctx->SaveTensorForBackward(inputs.at(1));   // label
  ctx->SaveTensorForBackward(outputs.at(1));  // prob

  return Maybe<void>::Ok();
}

Maybe<void> SoftmaxCrossEntropy::Apply(const SoftmaxCrossEntropyGradState* ctx,
                                       const TensorTuple& out_grads, TensorTuple* in_grads) const {
  if (!ctx->requires_grad) { return Maybe<void>::Ok(); }

  CHECK_EQ_OR_RETURN(out_grads.size(), 2);  // out, prob(no grad)
  const auto& dy = out_grads.at(0);
  const auto& label = ctx->SavedTensors().at(0);
  const auto& prob = ctx->SavedTensors().at(1);

  in_grads->resize(2);  // prediction, label
  (*in_grads)[0] = JUST(functional::SoftmaxCrossEntropyGrad(dy, label, prob));
  return Maybe<void>::Ok();
}

REGISTER_OP_EXPR_GRAD_FUNCTION("softmax_cross_entropy", SoftmaxCrossEntropy);

}  // namespace one
}  // namespace oneflow
