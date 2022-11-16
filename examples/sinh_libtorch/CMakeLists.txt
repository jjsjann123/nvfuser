cmake_minimum_required(VERSION 3.10 FATAL_ERROR)
project(sinh_example LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 14)

list(APPEND CMAKE_PREFIX_PATH "/opt/pytorch/pytorch/torch/" "/opt/pytorch/pytorch/third_party/nvfuser/build")

find_package(Torch REQUIRED)
find_package(nvfuser REQUIRED)

add_executable(sinh_example main.cpp)
target_link_libraries(sinh_example ${TORCH_LIBRARIES} nvfuser)
