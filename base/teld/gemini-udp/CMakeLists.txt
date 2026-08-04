add_executable(rts2-teld-gemini-udp gemini_udp.cpp)
target_link_libraries(rts2-teld-gemini-udp PRIVATE base_teld base_kernel)
