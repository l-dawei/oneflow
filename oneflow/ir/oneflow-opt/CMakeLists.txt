get_property(dialect_libs GLOBAL PROPERTY MLIR_DIALECT_LIBS)
get_property(conversion_libs GLOBAL PROPERTY MLIR_CONVERSION_LIBS)
add_llvm_executable(oneflow-opt oneflow-opt.cpp)

set(_origin_prefix "\$ORIGIN")
if(APPLE)
  set(_origin_prefix "@loader_path")
endif()
set_target_properties(
  oneflow-opt PROPERTIES BUILD_WITH_INSTALL_RPATH OFF BUILD_RPATH "${_origin_prefix}"
                         INSTALL_RPATH "${_origin_prefix}")
llvm_update_compile_flags(oneflow-opt)
target_link_libraries(oneflow-opt PRIVATE MLIROneFlow ${dialect_libs} ${conversion_libs} MLIROptLib)

mlir_check_all_link_libraries(oneflow-opt)
