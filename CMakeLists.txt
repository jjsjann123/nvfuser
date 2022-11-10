cmake_minimum_required(VERSION 3.0)
project(nvfuser)

# ---
set(NVFUSER_ROOT ${PROJECT_SOURCE_DIR})


# --- hacky variables
set(MKL_THREADING CACHE STRING "gnu_thread")
set(MKL_ROOT CACHE STRING "/opt/conda/lib/")
set(TORCH_CUDA_ARCH_LIST CACHE STRING "8.0")
option(USE_ROCM "Use ROCM" OFF)
set(TORCH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
list(APPEND CMAKE_PREFIX_PATH "/opt/pytorch/pytorch/torch/")
link_directories(/opt/conda/lib/)
set(CMAKE_BINARY_DIR "${NVFUSER_ROOT}/build")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}")

# --- dependencies
find_package(Torch REQUIRED)
find_package(OpenMP REQUIRED)

# --- [ fake project

add_executable(${PROJECT_NAME} toy.cpp)
target_link_libraries(toy PRIVATE ${TORCH_LIBRARIES})
target_compile_options(toy PRIVATE ${OpenMP_CXX_OPTIONS})
target_link_libraries(toy PRIVATE ${OpenMP_CXX_LIBRARIES})
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 14)

# --- generate runtime files

if(USE_ROCM)
  set(TORCHLIB_FLAVOR torch_hip)
else()
  set(TORCHLIB_FLAVOR torch_cuda)
endif()

# The list of NVFUSER runtime files
list(APPEND NVFUSER_RUNTIME_FILES
  ${NVFUSER_ROOT}/runtime/array.cu
  ${NVFUSER_ROOT}/runtime/block_reduction.cu
  ${NVFUSER_ROOT}/runtime/block_sync_atomic.cu
  ${NVFUSER_ROOT}/runtime/block_sync_default.cu
  ${NVFUSER_ROOT}/runtime/broadcast.cu
  ${NVFUSER_ROOT}/runtime/fp16_support.cu
  ${NVFUSER_ROOT}/runtime/fused_reduction.cu
  ${NVFUSER_ROOT}/runtime/fused_welford_helper.cu
  ${NVFUSER_ROOT}/runtime/fused_welford_impl.cu
  ${NVFUSER_ROOT}/runtime/bf16_support.cu
  ${NVFUSER_ROOT}/runtime/grid_broadcast.cu
  ${NVFUSER_ROOT}/runtime/grid_reduction.cu
  ${NVFUSER_ROOT}/runtime/grid_sync.cu
  ${NVFUSER_ROOT}/runtime/helpers.cu
  ${NVFUSER_ROOT}/runtime/index_utils.cu
  ${NVFUSER_ROOT}/runtime/random_numbers.cu
  ${NVFUSER_ROOT}/runtime/tensor.cu
  ${NVFUSER_ROOT}/runtime/tuple.cu
  ${NVFUSER_ROOT}/runtime/type_traits.cu
  ${NVFUSER_ROOT}/runtime/welford.cu
  ${NVFUSER_ROOT}/runtime/warp.cu
  ${NVFUSER_ROOT}/runtime/tensorcore.cu
  ${NVFUSER_ROOT}/runtime/memory.cu
  ${TORCH_ROOT}/aten/src/ATen/cuda/detail/PhiloxCudaStateRaw.cuh
  ${TORCH_ROOT}/aten/src/ATen/cuda/detail/UnpackRaw.cuh
)

if(USE_ROCM)
list(APPEND NVFUSER_RUNTIME_FILES
  ${NVFUSER_ROOT}/runtime/array_rocm.cu
  ${NVFUSER_ROOT}/runtime/bf16_support_rocm.cu
  ${NVFUSER_ROOT}/runtime/block_sync_default_rocm.cu
  ${NVFUSER_ROOT}/runtime/warp_rocm.cu
)
endif()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/include/nvfuser_resources")

# "stringify" NVFUSER runtime sources
# (generate C++ header files embedding the original input as a string literal)
set(NVFUSER_STRINGIFY_TOOL "${NVFUSER_ROOT}/tools/stringify_file.py")
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
  add_dependencies(${PROJECT_NAME} nvfuser_rt_${filename})

  # also generate the resource headers during the configuration step
  # (so tools like clang-tidy can run w/o requiring a real build)
  execute_process(COMMAND
    ${PYTHON_EXECUTABLE} ${NVFUSER_STRINGIFY_TOOL} -i ${src} -o ${dst})
endforeach()

target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_BINARY_DIR}/include")

