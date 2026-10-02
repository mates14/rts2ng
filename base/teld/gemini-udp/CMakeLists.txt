add_executable(rts2-teld-gemini-udp gemini_udp.cpp)
target_link_libraries(rts2-teld-gemini-udp PRIVATE base_teld base_kernel)

# the same driver plus the MPU-9250 IMU position check, see the base note at
# the top of gemini_udp_imu.cpp - a separate source so the plain driver
# stays untouched until the IMU has proven itself
add_executable(rts2-teld-gemini-udp-imu gemini_udp_imu.cpp)
target_link_libraries(rts2-teld-gemini-udp-imu PRIVATE base_teld base_kernel)
