cmake_minimum_required(VERSION 3.0 FATAL_ERROR)
project(toy)

find_package(Torch REQUIRED)

add_executable(toy toy.cpp)
target_link_libraries(toy "${TORCH_LIBRARIES}")
set_property(TARGET toy PROPERTY CXX_STANDARD 14)
