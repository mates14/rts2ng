add_library(base_teld STATIC
	src/teld.cpp
	src/dut1.cpp
	src/pluto/observe.cpp
	src/tgdrive.cpp
	src/connremotes.cpp
	src/fork.cpp
	src/hms.cpp
	src/tellx200.cpp
	src/gem.cpp
	src/geminicaringloop.cpp
	src/tpointmodel.cpp
	src/tpointmodelterm.cpp
)

target_include_directories(base_teld PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

# needed for GeminiCaringLoop's std::thread/mutex/condition_variable
find_package(Threads REQUIRED)

target_link_libraries(base_teld PUBLIC base_kernel Threads::Threads)

add_subdirectory(dummy)
add_subdirectory(d50)
add_subdirectory(gemini)
add_subdirectory(gemini-udp)
add_subdirectory(lx200)

if(PARACL_LIBMKS3_C AND PARACL_LIBMKS3_H)
	add_subdirectory(paramount)
endif()
