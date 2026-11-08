add_executable(rts2-teld-d50 d50.cpp d50path.cpp wormencoder.cpp)
target_link_libraries(rts2-teld-d50 PRIVATE base_teld base_kernel)
