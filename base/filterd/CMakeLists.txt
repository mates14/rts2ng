add_library(base_filterd STATIC
	src/filterd.cpp
)

target_include_directories(base_filterd PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(base_filterd PUBLIC base_kernel)

base_driver(dummy "Simulated filter wheel")

if(BASE_HAVE_FLI)
	add_subdirectory(fli)
endif()
