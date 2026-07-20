add_library(base_focusd STATIC
	src/focusd.cpp
)

target_include_directories(base_focusd PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(base_focusd PUBLIC base_kernel)

add_subdirectory(dummy)
add_subdirectory(tefo)
add_subdirectory(aaf)
add_subdirectory(optec)
add_subdirectory(nstep)

if(FLI_INCLUDE_DIR AND FLI_LIBRARY)
	add_subdirectory(fli)
endif()
