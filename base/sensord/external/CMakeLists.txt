add_executable(rts2-sensor-external external.cpp)
target_link_libraries(rts2-sensor-external PRIVATE base_sensord base_kernel)
