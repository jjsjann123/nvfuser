rm CMakeCache.txt; rm toy; cmake -DMKL_ROOT=/opt/conda/lib/ -DMKL_THREADING=gnu_thread -DCMAKE_PREFIX_PATH=/opt/pytorch/pytorch/torch/ -DTORCH_CUDA_ARCH_LIST=8.0 .; cmake --build . --verbose; ./toy
