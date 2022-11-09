cmake_minimum_required(VERSION 3.0 FATAL_ERROR)
project(toy)

find_package(Torch REQUIRED)
find_package(OpenMP REQUIRED)

link_directories(/opt/conda/lib/)
add_executable(toy toy.cpp)

target_link_libraries(toy PRIVATE ${TORCH_LIBRARIES})
target_compile_options(toy PRIVATE ${OpenMP_CXX_OPTIONS})
target_link_libraries(toy PRIVATE ${OpenMP_CXX_LIBRARIES})
set_property(TARGET toy PROPERTY CXX_STANDARD 14)
