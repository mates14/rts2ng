add_executable(rts2-camd-v4l v4l.cpp)
target_link_libraries(rts2-camd-v4l PRIVATE base_camd base_kernel)
