add_executable(rts2-camd-dummy dummy.cpp)
target_link_libraries(rts2-camd-dummy PRIVATE base_camd base_kernel)
