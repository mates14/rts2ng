# rts2-bopwatch - helper of the D50 dome bench (d50-bench/), not installed
add_executable(rts2-bopwatch d50-bench/bopwatch.cpp)
target_link_libraries(rts2-bopwatch PRIVATE base_kernel)
