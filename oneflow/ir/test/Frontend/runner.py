import oneflow as flow
print(flow.__file__)

from oneflow.nn.graph.compiler import Runner


class RELU(flow.nn.Module):
    def __init__(self):
        super().__init__()
        self.relu = flow.nn.ReLU()

    def forward(self, x):
        return self.relu(x)

class GraphModule(flow.nn.Graph):
    def __init__(self):
        super().__init__()
        self.fw = RELU()

    def build(self, x):
        return self.fw(x)


func = Runner(GraphModule)
input = flow.Tensor([-1, 1.])
output = func(input)
print(output)

import numpy as np
func = Runner(GraphModule)
input = flow.Tensor([-1, 1., -2])
output = func(input)
print(output)