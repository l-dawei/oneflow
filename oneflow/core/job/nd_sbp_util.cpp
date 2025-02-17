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

#include "oneflow/core/job/nd_sbp_util.h"
#include "oneflow/core/common/balanced_splitter.h"
#include "oneflow/core/common/nd_index_offset_helper.h"

namespace oneflow {
namespace {
// Go through all the ranks while transfer between two nd sbps with no PartialSum under the same
// placement.
// NOTE: We need to make sure no partial sums in the sbps of the producer and consumer.
void DfsTraverseRanks4NdSbp(
    int32_t depth, std::vector<int64_t>& in_parallel_ids,
    const std::vector<int64_t>& out_parallel_ids, const Shape& parallel_hierarchy,
    const NdIndexOffsetHelper<int64_t, SHAPE_MAX_AXIS_SIZE>& hierarchy_index_helper,
    const NdSbp& in_nd_sbp, const std::function<void(int32_t)>& visit) {
  if (depth >= parallel_hierarchy.NumAxes()) {
    visit(hierarchy_index_helper.NdIndexToOffset(in_parallel_ids.data(),
                                                 parallel_hierarchy.NumAxes()));
    return;
  }
  if (in_nd_sbp.sbp_parallel(depth).has_broadcast_parallel()) {
    // If Broadcast in the sbp of the producer, only visit those ranks with the same id as the
    // current rank along the depth-dimension.
    in_parallel_ids[depth] = out_parallel_ids[depth];
    DfsTraverseRanks4NdSbp(depth + 1, in_parallel_ids, out_parallel_ids, parallel_hierarchy,
                           hierarchy_index_helper, in_nd_sbp, visit);
  } else {
    // If Split or PartialSum, go through all the ranks along the depth-dimension.
    for (int64_t i = 0; i < parallel_hierarchy.dim_vec().at(depth); i++) {
      in_parallel_ids[depth] = i;
      DfsTraverseRanks4NdSbp(depth + 1, in_parallel_ids, out_parallel_ids, parallel_hierarchy,
                             hierarchy_index_helper, in_nd_sbp, visit);
    }
  }
}

void DfsTraverse4NdSbp(int64_t recv_id, const std::shared_ptr<Shape>& parallel_hierarchy,
                       const NdSbp& in_nd_sbp, const std::function<void(int32_t)>& visit) {
  int32_t hierarchy_dimension = parallel_hierarchy->NumAxes();
  const NdIndexOffsetHelper<int64_t, SHAPE_MAX_AXIS_SIZE> hierarchy_index_helper(
      parallel_hierarchy->dim_vec().data(), hierarchy_dimension);
  std::vector<int64_t> in_parallel_ids(hierarchy_dimension);
  std::vector<int64_t> out_parallel_ids(hierarchy_dimension);
  hierarchy_index_helper.OffsetToNdIndex(recv_id, out_parallel_ids.data(), hierarchy_dimension);
  DfsTraverseRanks4NdSbp(0, in_parallel_ids, out_parallel_ids, *parallel_hierarchy,
                         hierarchy_index_helper, in_nd_sbp, visit);
}
}  // namespace

std::vector<TensorSliceView> GetTensorSliceView(const int64_t parallel_num,
                                                const SbpParallel& sbp_parallel,
                                                const BlobDesc& blob_desc) {
  const Shape& shape = blob_desc.shape();
  std::vector<Range> ranges(shape.NumAxes());
  FOR_RANGE(int64_t, i, 0, shape.NumAxes()) {
    ranges[i].mut_begin() = 0;
    ranges[i].mut_end() = shape.At(i);
  }
  if (shape.NumAxes() == 0 && shape.elem_cnt() == 1) {
    // NOTE(chengcheng): For Scalar Tensor.
    ranges.emplace_back(0, 1);
  }
  std::vector<TensorSliceView> views;
  views.reserve(parallel_num);
  if (sbp_parallel.has_partial_sum_parallel() || sbp_parallel.has_broadcast_parallel()) {
    FOR_RANGE(int64_t, i, 0, parallel_num) { views.emplace_back(ranges); }
  } else if (sbp_parallel.has_split_parallel()) {
    const int64_t axis = sbp_parallel.split_parallel().axis();
    CHECK_LT(axis, shape.NumAxes());
    const BalancedSplitter bs(shape.At(axis), parallel_num);
    FOR_RANGE(int64_t, i, 0, parallel_num) {
      if (bs.At(i).size() == 0) {
        views.emplace_back();
      } else {
        ranges[axis] = bs.At(i);
        views.emplace_back(ranges);
      }
    }
  } else {
    UNIMPLEMENTED();
  }
  return views;
}

TensorSliceView GetTensorSliceView4ParallelRank(const Shape& parallel_hierarchy,
                                                const NdSbp& nd_sbp, const Shape& logical_shape,
                                                const std::vector<int64_t>& parallel_rank) {
  std::vector<Range> ranges(logical_shape.NumAxes());
  FOR_RANGE(int64_t, i, 0, logical_shape.NumAxes()) {
    ranges[i].mut_begin() = 0;
    ranges[i].mut_end() = logical_shape.At(i);
  }
  if (logical_shape.NumAxes() == 0 && logical_shape.elem_cnt() == 1) {
    // NOTE(chengcheng): For Scalar Tensor.
    ranges.emplace_back(0, 1);
  }
  if (parallel_hierarchy.elem_cnt() == 1) { return TensorSliceView(ranges); }
  if (parallel_hierarchy.NumAxes() == 1) {
    const SbpParallel& sbp_parallel = nd_sbp.sbp_parallel(0);
    if (sbp_parallel.has_split_parallel()) {
      const int64_t split_axis = sbp_parallel.split_parallel().axis();
      CHECK_GE(split_axis, 0);
      CHECK_LT(split_axis, ranges.size());
      const int64_t id = parallel_rank.front();
      CHECK_GE(id, 0);
      CHECK_LT(id, parallel_hierarchy.elem_cnt());
      const BalancedSplitter bs(logical_shape.At(split_axis), parallel_hierarchy.elem_cnt());
      CHECK_GT(bs.At(id).size(), 0);
      ranges[split_axis] = bs.At(id);
    }
  } else {
    FOR_RANGE(int64_t, i, 0, parallel_hierarchy.NumAxes()) {
      const SbpParallel& sbp_parallel = nd_sbp.sbp_parallel(i);
      if (sbp_parallel.has_split_parallel()) {
        const int64_t split_axis = sbp_parallel.split_parallel().axis();
        CHECK_GE(split_axis, 0);
        CHECK_LT(split_axis, ranges.size());
        CHECK_EQ(ranges[split_axis].size() % parallel_hierarchy.At(i), 0);
        const int64_t range_size = ranges[split_axis].size() / parallel_hierarchy.At(i);
        const int64_t dim_start = ranges[split_axis].begin() + parallel_rank.at(i) * range_size;
        ranges[split_axis].mut_begin() = dim_start;
        ranges[split_axis].mut_end() = dim_start + range_size;
      }
    }
  }
  return TensorSliceView(ranges);
}

TensorSliceView GetTensorSliceView4ParallelId(const Shape& parallel_hierarchy, const NdSbp& nd_sbp,
                                              const Shape& logical_shape, int64_t parallel_id) {
  NdIndexOffsetHelper<int64_t, SHAPE_MAX_AXIS_SIZE> hierarchy_index_helper(
      parallel_hierarchy.dim_vec().data(), parallel_hierarchy.NumAxes());
  std::vector<int64_t> parallel_rank(SHAPE_MAX_AXIS_SIZE);
  hierarchy_index_helper.OffsetToNdIndex(parallel_id, parallel_rank.data());
  return GetTensorSliceView4ParallelRank(parallel_hierarchy, nd_sbp, logical_shape, parallel_rank);
}

std::vector<TensorSliceView> GetTensorSliceView(const Shape& parallel_hierarchy,
                                                const NdSbp& nd_sbp, const Shape& logical_shape) {
  std::vector<TensorSliceView> views;
  views.reserve(parallel_hierarchy.elem_cnt());
  FOR_RANGE(int64_t, i, 0, parallel_hierarchy.elem_cnt()) {
    views.emplace_back(GetTensorSliceView4ParallelId(parallel_hierarchy, nd_sbp, logical_shape, i));
  }
  return views;
}

TensorSliceView GetBroadcastTensorSliceView(const BlobDesc& blob_desc) {
  return TensorSliceView(blob_desc.shape());
}

bool NdSbpHasPartialParallel(const NdSbp& nd_sbp) {
  CHECK_GT(nd_sbp.sbp_parallel_size(), 0);
  FOR_RANGE(int64_t, i, 0, nd_sbp.sbp_parallel_size()) {
    if (nd_sbp.sbp_parallel(i).has_partial_sum_parallel()) { return true; }
  }
  return false;
}

bool NdSbpHasBroadcastParallel(const NdSbp& nd_sbp) {
  CHECK_GT(nd_sbp.sbp_parallel_size(), 0);
  FOR_RANGE(int64_t, i, 0, nd_sbp.sbp_parallel_size()) {
    if (nd_sbp.sbp_parallel(i).has_broadcast_parallel()) { return true; }
  }
  return false;
}

bool NdSbpIsAllBroadcast(const NdSbp& nd_sbp) {
  for (const auto& sbp_parallel : nd_sbp.sbp_parallel()) {
    if (!sbp_parallel.has_broadcast_parallel()) { return false; }
  }
  return true;
}

bool NdSbpIsAllPartialSum(const NdSbp& nd_sbp) {
  for (const auto& sbp_parallel : nd_sbp.sbp_parallel()) {
    if (!sbp_parallel.has_partial_sum_parallel()) { return false; }
  }
  return true;
}

bool NdSbpIsAllSplit(const NdSbp& nd_sbp, int64_t axis) {
  for (const auto& sbp_parallel : nd_sbp.sbp_parallel()) {
    if (!(sbp_parallel.has_split_parallel() && sbp_parallel.split_parallel().axis() == axis)) {
      return false;
    }
  }
  return true;
}

void GetRankSendRecvIntersection(int64_t parallel_id,
                                 const std::shared_ptr<Shape>& parallel_hierarchy,
                                 const NdSbp& src_nd_sbp, const NdSbp& dst_nd_sbp,
                                 const Shape& logical_shape,
                                 std::vector<TensorSliceView>* send_intersections,
                                 std::vector<TensorSliceView>* recv_intersections) {
  CHECK(parallel_hierarchy != nullptr);
  const int64_t parallel_num = parallel_hierarchy->elem_cnt();
  CHECK_LT(parallel_id, parallel_num);

  const std::vector<TensorSliceView>& in_slices =
      GetTensorSliceView(*parallel_hierarchy, src_nd_sbp, logical_shape);
  const std::vector<TensorSliceView>& out_slices =
      GetTensorSliceView(*parallel_hierarchy, dst_nd_sbp, logical_shape);

  // cur rank recv from
  recv_intersections->resize(parallel_num);
  const TensorSliceView& cur_rank_out_slice = out_slices.at(parallel_id);
  const auto& add_to_recv_intersections = [&](int32_t send_id) {
    const TensorSliceView& in_slice = in_slices.at(send_id);
    const TensorSliceView& intersection = cur_rank_out_slice.Intersect(in_slice);
    if (intersection.IsEmpty()) { return; }
    recv_intersections->at(send_id) = intersection;
  };
  DfsTraverse4NdSbp(parallel_id, parallel_hierarchy, src_nd_sbp, add_to_recv_intersections);

  // cur rank send to
  send_intersections->resize(parallel_num);
  const TensorSliceView& cur_rank_in_slice = in_slices.at(parallel_id);
  for (int64_t recv_i = 0; recv_i < parallel_num; ++recv_i) {
    const auto& add_to_send_intersections = [&](int32_t send_id) {
      if (send_id != parallel_id) { return; }
      const TensorSliceView& out_slice = out_slices.at(recv_i);
      const TensorSliceView& intersection = out_slice.Intersect(cur_rank_in_slice);
      if (intersection.IsEmpty()) { return; }
      send_intersections->at(recv_i) = intersection;
    };
    DfsTraverse4NdSbp(recv_i, parallel_hierarchy, src_nd_sbp, add_to_send_intersections);
  }
}

}  // namespace oneflow
