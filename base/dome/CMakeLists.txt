add_library(base_dome STATIC
	src/dome.cpp
	src/connford.cpp
	src/domeford.cpp
)

target_include_directories(base_dome PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(base_dome PUBLIC base_kernel)

base_driver(d50 "D50 dome")
base_driver(bart "BART dome")
base_driver(zelio "Zelio PLC dome")
