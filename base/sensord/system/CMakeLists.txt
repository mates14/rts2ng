add_executable(rts2-sensor-system system.cpp)
target_link_libraries(rts2-sensor-system PRIVATE base_sensord base_kernel)
