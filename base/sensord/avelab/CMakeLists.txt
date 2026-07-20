add_executable(rts2-sensor-avelab avelab.cpp)
target_link_libraries(rts2-sensor-avelab PRIVATE base_sensord base_kernel)
