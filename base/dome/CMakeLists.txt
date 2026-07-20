add_library(base_dome STATIC
	src/dome.cpp
	src/connford.cpp
	src/domeford.cpp
)

target_include_directories(base_dome PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(base_dome PUBLIC base_kernel)

add_subdirectory(d50)
add_subdirectory(bart)
add_subdirectory(zelio)
