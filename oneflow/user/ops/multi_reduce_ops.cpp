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
#include "oneflow/core/framework/framework.h"
#include "oneflow/core/framework/op_generated.h"

namespace oneflow {

Maybe<void> MultiReduceSumPowAbsOp::GetSbp(user_op::SbpContext* ctx) {
  int64_t min_num_axes = ctx->LogicalTensorDesc4InputArgNameAndIndex("x", 0).shape().NumAxes();
  for (int64_t i = 1; i < ctx->user_op_conf().input_size("x"); ++i) {
    min_num_axes = std::min(min_num_axes,
                            ctx->LogicalTensorDesc4InputArgNameAndIndex("x", i).shape().NumAxes());
  }
  for (int64_t i = 0; i < min_num_axes; ++i) {
    ctx->NewBuilder().Split(ctx->inputs(), i).PartialSum(user_op::OpArg("y", 0)).Build();
  }
  return Maybe<void>::Ok();
}

Maybe<void> MultiReduceSumPowAbsOp::InferLogicalTensorDesc(user_op::InferContext* ctx) {
  user_op::TensorDesc* y = ctx->OutputTensorDesc("y", 0);
  *y->mut_shape() = Shape({1});
  return Maybe<void>::Ok();
}

Maybe<void> MultiReduceSumPowAbsOp::InferPhysicalTensorDesc(user_op::InferContext* ctx) {
  return InferLogicalTensorDesc(ctx);
}

Maybe<void> MultiReduceSumPowAbsOp::InferDataType(user_op::InferContext* ctx) {
  const user_op::TensorDesc& x_0 = ctx->InputTensorDesc("x", 0);
  user_op::TensorDesc* y = ctx->OutputTensorDesc("y", 0);
  for (int64_t i = 1; i < ctx->input_size("x"); ++i) {
    const user_op::TensorDesc& x_i = ctx->InputTensorDesc("x", i);
    CHECK_EQ_OR_RETURN(x_i.data_type(), x_0.data_type());
  }
  *y->mut_data_type() = x_0.data_type();
  return Maybe<void>::Ok();
}

Maybe<void> MultiReduceSumPowAbsOp::CheckAttr(const user_op::UserOpDefWrapper&,
                                              const user_op::UserOpConfWrapper& op_conf) {
  CHECK_OR_RETURN(op_conf.input_size("x") >= 1);
  return Maybe<void>::Ok();
}

}  // namespace oneflow
