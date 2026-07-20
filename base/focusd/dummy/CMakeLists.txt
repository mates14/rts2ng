add_executable(rts2-focusd-dummy dummy.cpp)
target_link_libraries(rts2-focusd-dummy PRIVATE base_focusd base_kernel)
