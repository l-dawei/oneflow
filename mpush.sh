git push test start-serial 
cd build 
cmake .. -C ../cmake/caches/cn/cuda.cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_RDMA=ON

make -j$(nproc)