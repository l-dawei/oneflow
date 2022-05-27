"""
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
"""

import unittest
from collections import OrderedDict
import oneflow
import numpy as np
import oneflow as flow
import oneflow.unittest
from oneflow.test_utils.test_util import GenArgList

from oneflow.test_utils.automated_test_util import *
import time
import os

os.environ["ONEFLOW_BOXING_DISABLE_MIDDLE_NODE_AND_CHECK"] = "1"
os.environ["LOGICAL_SR"] = "1"


def _test_nccl_logical_send_recv(test_case, src_nd_sbp, dst_nd_sbp):
    # can not process p in dst
    if flow.sbp.partial_sum() in dst_nd_sbp:
        return

    # skip src == dst
    if src_nd_sbp == dst_nd_sbp:
        return

    # in this case, use intra group boxing
    if src_nd_sbp[0] == dst_nd_sbp[0]:
        return

    # in this case, use inter group boxing
    if (
        src_nd_sbp[1] == dst_nd_sbp[1]
        and src_nd_sbp[0] != src_nd_sbp[1]
        and src_nd_sbp[0] != src_nd_sbp[1]
    ):
        return

    # in this case, use 1d boxing
    if src_nd_sbp[0] == src_nd_sbp[1] and dst_nd_sbp[0] == dst_nd_sbp[1]:
        return

    # input
    placement = flow.placement("cuda", ranks=[[0, 1], [2, 3]])
    in_np = np.arange(4 * 4 * 4).reshape(4, 4, 4)
    x = flow.tensor(
        in_np,
        sbp=src_nd_sbp,
        placement=placement,
    )

    # check eager boxing
    test_case.assertTrue(np.array_equal(x.numpy(), in_np))
    eager_out = x.to_global(sbp=dst_nd_sbp, placement=placement)
    assert np.array_equal(eager_out.numpy(), in_np)
    
    # bad case of graph: S with P
    if src_nd_sbp[0] == flow.sbp.partial_sum and src_nd_sbp[1] == flow.sbp.split(0):
        return
    if src_nd_sbp[0] == flow.sbp.partial_sum and src_nd_sbp[1] == flow.sbp.split(1):
        return
    if src_nd_sbp[0] == flow.sbp.partial_sum and src_nd_sbp[1] == flow.sbp.split(2):
        return
    if src_nd_sbp[0] == flow.sbp.split(0) and src_nd_sbp[1] == flow.sbp.partial_sum:
        return
    if src_nd_sbp[0] == flow.sbp.split(1) and src_nd_sbp[1] == flow.sbp.partial_sum:
        return
    if src_nd_sbp[0] == flow.sbp.split(2) and src_nd_sbp[1] == flow.sbp.partial_sum:
        return
    # bad case of graph: diff S
    if src_nd_sbp[0] == flow.sbp.split(0) and src_nd_sbp[1] == flow.sbp.split(1):
        return
    if src_nd_sbp[0] == flow.sbp.split(0) and src_nd_sbp[1] == flow.sbp.split(2):
        return
    if src_nd_sbp[0] == flow.sbp.split(1) and src_nd_sbp[1] == flow.sbp.split(2):
        return
    if src_nd_sbp[0] == flow.sbp.split(1) and src_nd_sbp[1] == flow.sbp.split(0):
        return
    if src_nd_sbp[0] == flow.sbp.split(2) and src_nd_sbp[1] == flow.sbp.split(0):
        return
    if src_nd_sbp[0] == flow.sbp.split(2) and src_nd_sbp[1] == flow.sbp.split(1):
        return

	# check graph boxing
    flow.boxing.nccl.enable_use_compute_stream(True)
    class TestNcclLogicalSendRecvGraph(flow.nn.Graph):
        def __init__(self):
            super().__init__()

        def build(self, x):
            # from src nd sbp to dst nd sbp
            y = x.to_global(sbp=dst_nd_sbp, placement=placement)
            return y
    graph = TestNcclLogicalSendRecvGraph()
    y = graph(x)
    out_np = y.numpy()
    equal = np.array_equal(out_np, in_np)
    if not equal:
        print("graph repr:\n", graph)
        print("in np:\n", in_np)
        print("diff np:\n", out_np - in_np)
    test_case.assertTrue(equal)



def gen_nd_sbp():
    sbp_list = [
        flow.sbp.partial_sum(),
        flow.sbp.broadcast(),
        flow.sbp.split(0),
        flow.sbp.split(1),
        flow.sbp.split(2),
    ]
    nd_sbp_list = []
    for sbp0 in sbp_list:
        for sbp1 in sbp_list:
            nd_sbp_list.append([sbp0, sbp1])
    return nd_sbp_list


@flow.unittest.skip_unless_1n4d()
@unittest.skipIf(os.getenv("ONEFLOW_TEST_CPU_ONLY"), "only test cpu cases")
class TestNcclLogicalSendRecv(flow.unittest.TestCase):
    def test_nccl_logical_send_recv(test_case):
        arg_dict = OrderedDict()
        arg_dict["src_nd_sbp"] = gen_nd_sbp()
        arg_dict["dst_nd_sbp"] = gen_nd_sbp()
        for arg in GenArgList(arg_dict):
            _test_nccl_logical_send_recv(test_case, *arg)


if __name__ == "__main__":
    unittest.main()