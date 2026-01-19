rm -rf build && cmake -G Ninja -B build -DOPENVDS_SINGLE_THREADED=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build
