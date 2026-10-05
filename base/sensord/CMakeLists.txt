add_library(base_sensord STATIC
	src/sensord.cpp
)

target_include_directories(base_sensord PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(base_sensord PUBLIC base_kernel)

base_driver(system "Host system status")
base_driver(bart-rain "BART rain sensor")
base_driver(cloud4 "Cloud4 cloud sensor")
base_driver(aws-mlab "AWS mlab weather station")
base_driver(d50-wfunit "D50 weather unit")
base_driver(makak-shutter "Makak camera shutter/heating unit")
base_driver(nut "UPS via NUT")
base_driver(external "External-program sensor")
base_driver(fram-weather "FRAM weather station")
base_driver(avelab "Avelab sensor")
base_driver(windrs "WindRS wind sensor")
