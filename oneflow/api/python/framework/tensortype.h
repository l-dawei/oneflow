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
#ifndef ONEFLOW_API_PYTHON_FRAMEWORK_TENSORTYPE_H_
#define ONEFLOW_API_PYTHON_FRAMEWORK_TENSORTYPE_H_

#include <Python.h>
#include "oneflow/core/framework/dtype.h"
#include "oneflow/core/framework/device.h"

namespace oneflow {
namespace one {

typedef struct {
  PyTypeObject py_type;
  char name[64];
  bool is_cuda;
  DataType datatype;
  DeviceType device;
} PyTensortype;

bool PyTensortype_Check(PyObject*);

inline DeviceType PyTensortype_UnpackDevice(PyObject* self) {
  return ((PyTensortype*)self)->device;
}
inline Symbol<DType> PyTensortype_UnpackDType(PyObject* self) {
  return CHECK_JUST(DType::Get(((PyTensortype*)self)->datatype));
}

PyObject* PyTensortype_FromDTypeDeviceType(DataType, DeviceType);
PyObject* PyTensortype_FromString(const std::string&);

}  // namespace one
}  // namespace oneflow

#endif  // ONEFLOW_API_PYTHON_FRAMEWORK_TENSOR_H_
