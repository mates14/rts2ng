add_executable(rts2-sensor-aws-mlab aws-mlab.cpp)
target_link_libraries(rts2-sensor-aws-mlab PRIVATE base_sensord base_kernel)
