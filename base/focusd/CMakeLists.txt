add_library(base_focusd STATIC
	src/focusd.cpp
)

target_include_directories(base_focusd PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(base_focusd PUBLIC base_kernel)

base_driver(dummy "Simulated focuser")
base_driver(tefo "TEFO focuser")
base_driver(aaf "AAF focuser")
base_driver(optec "Optec focuser")
base_driver(nstep "NSTEP focuser")

if(BASE_HAVE_FLI)
	add_subdirectory(fli)
endif()
