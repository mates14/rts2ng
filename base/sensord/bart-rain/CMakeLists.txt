add_executable(rts2-sensor-bart-rain bart-rain.cpp)
target_link_libraries(rts2-sensor-bart-rain PRIVATE base_sensord base_kernel)
