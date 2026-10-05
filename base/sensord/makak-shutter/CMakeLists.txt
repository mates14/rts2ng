add_executable(rts2-sensor-makak-shutter makak-shutter.cpp)
target_link_libraries(rts2-sensor-makak-shutter PRIVATE base_sensord base_kernel)
