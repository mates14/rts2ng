add_executable(rts2-teld-dummy dummy.cpp)
target_link_libraries(rts2-teld-dummy PRIVATE base_teld base_kernel)
