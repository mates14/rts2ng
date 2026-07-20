add_executable(rts2-sensor-fram-weather
	fram-weather.cpp
	framudp.cpp
)
target_include_directories(rts2-sensor-fram-weather PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rts2-sensor-fram-weather PRIVATE base_sensord base_kernel)
