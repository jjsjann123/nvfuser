if(USE_CUDA)
  set(TORCHLIB_FLAVOR torch_cuda)
elseif(USE_ROCM)
  set(TORCHLIB_FLAVOR torch_hip)
endif()

# The list of NVFUSER runtime files
list(APPEND NVFUSER_RUNTIME_FILES
  ${TORCH_ROOT}/third_party/nvfuser/runtime/array.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/block_reduction.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/block_sync_atomic.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/block_sync_default.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/broadcast.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/fp16_support.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/fused_reduction.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/fused_welford_helper.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/fused_welford_impl.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/bf16_support.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/grid_broadcast.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/grid_reduction.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/grid_sync.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/helpers.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/index_utils.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/random_numbers.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/tensor.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/tuple.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/type_traits.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/welford.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/warp.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/tensorcore.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/memory.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/swizzle.cu
  ${TORCH_ROOT}/aten/src/ATen/cuda/detail/PhiloxCudaStateRaw.cuh
  ${TORCH_ROOT}/aten/src/ATen/cuda/detail/UnpackRaw.cuh
)

if(USE_ROCM)
list(APPEND NVFUSER_RUNTIME_FILES
  ${TORCH_ROOT}/third_party/nvfuser/runtime/array_rocm.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/bf16_support_rocm.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/block_sync_default_rocm.cu
  ${TORCH_ROOT}/third_party/nvfuser/runtime/warp_rocm.cu
)
endif()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/include/nvfuser_resources")

# "stringify" NVFUSER runtime sources
# (generate C++ header files embedding the original input as a string literal)
set(NVFUSER_STRINGIFY_TOOL "${TORCH_ROOT}/third_party/nvfuser/tools/stringify_file.py")
foreach(src ${NVFUSER_RUNTIME_FILES})
  get_filename_component(filename ${src} NAME_WE)
  set(dst "${CMAKE_BINARY_DIR}/include/nvfuser_resources/${filename}.h")
  add_custom_command(
    COMMENT "Stringify NVFUSER runtime source file"
    OUTPUT ${dst}
    DEPENDS ${src} "${NVFUSER_STRINGIFY_TOOL}"
    COMMAND ${PYTHON_EXECUTABLE} ${NVFUSER_STRINGIFY_TOOL} -i ${src} -o ${dst}
  )
  add_custom_target(nvfuser_rt_${filename} DEPENDS ${dst})
  add_dependencies(${TORCHLIB_FLAVOR} nvfuser_rt_${filename})

  # also generate the resource headers during the configuration step
  # (so tools like clang-tidy can run w/o requiring a real build)
  execute_process(COMMAND
    ${PYTHON_EXECUTABLE} ${NVFUSER_STRINGIFY_TOOL} -i ${src} -o ${dst})
endforeach()

target_include_directories(${TORCHLIB_FLAVOR} PRIVATE "${CMAKE_BINARY_DIR}/include")
