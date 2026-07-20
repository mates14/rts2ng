add_executable(rts2-sensor-windrs windrs.cpp)
target_link_libraries(rts2-sensor-windrs PRIVATE base_sensord base_kernel)
