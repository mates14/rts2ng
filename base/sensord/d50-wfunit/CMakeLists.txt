add_executable(rts2-sensor-d50-wfunit d50-wfunit.cpp)
target_link_libraries(rts2-sensor-d50-wfunit PRIVATE base_sensord base_kernel)
