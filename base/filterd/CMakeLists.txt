add_library(base_filterd STATIC
	src/filterd.cpp
)

target_include_directories(base_filterd PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(base_filterd PUBLIC base_kernel)

add_subdirectory(dummy)

if(FLI_INCLUDE_DIR AND FLI_LIBRARY)
	add_subdirectory(fli)
endif()
