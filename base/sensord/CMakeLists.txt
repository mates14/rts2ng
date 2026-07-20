add_library(base_sensord STATIC
	src/sensord.cpp
)

target_include_directories(base_sensord PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(base_sensord PUBLIC base_kernel)

add_subdirectory(system)
add_subdirectory(bart-rain)
add_subdirectory(cloud4)
add_subdirectory(aws-mlab)
add_subdirectory(d50-wfunit)
add_subdirectory(nut)
add_subdirectory(external)
add_subdirectory(fram-weather)
add_subdirectory(avelab)
add_subdirectory(windrs)
