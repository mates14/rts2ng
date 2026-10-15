add_executable(rts2-sensor-scheduler scheduler.cpp)
target_link_libraries(rts2-sensor-scheduler PRIVATE base_sensord base_kernel)
