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
	src/mpu9250.cpp
	src/tpointmodel.cpp
	src/tpointmodelterm.cpp
)

target_include_directories(base_teld PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

# needed for GeminiCaringLoop's std::thread/mutex/condition_variable
find_package(Threads REQUIRED)

target_link_libraries(base_teld PUBLIC base_kernel Threads::Threads)

add_subdirectory(tests)

base_driver(dummy "Simulated telescope")
base_driver(d50 "D50 telescope")
base_driver(gemini "Losmandy Gemini")
base_driver(gemini-udp "Losmandy Gemini over UDP")
base_driver(lx200 "Meade LX200 protocol")

if(BASE_HAVE_PARACL)
	add_subdirectory(paramount)
endif()
