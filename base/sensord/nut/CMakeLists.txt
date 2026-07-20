add_executable(rts2-sensor-nut nut.cpp)
target_link_libraries(rts2-sensor-nut PRIVATE base_sensord base_kernel)
