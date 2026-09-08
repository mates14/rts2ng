# libnova ships no pkg-config file on this system - find it directly (same
# lookup as kernel/CMakeLists.txt; centrald.cpp needs libnova for sun/moon
# ephemeris calculations).
find_path(LIBNOVA_INCLUDE_DIR NAMES libnova/libnova.h)
find_library(LIBNOVA_LIBRARY NAMES nova)
if(NOT LIBNOVA_INCLUDE_DIR OR NOT LIBNOVA_LIBRARY)
	message(FATAL_ERROR "libnova development files not found")
endif()

add_executable(rts2-centrald src/centrald.cpp)

target_include_directories(rts2-centrald PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${LIBNOVA_INCLUDE_DIR}
)

target_link_libraries(rts2-centrald PRIVATE base_kernel ${LIBNOVA_LIBRARY})

# rts2-state answers the same day/night question centrald does, from the
# same riseset.cpp code and the same rts2.ini, but offline - hence it lives
# here next to centrald rather than in a tools directory of its own.
add_executable(rts2-state src/stateapp.cpp)

target_include_directories(rts2-state PRIVATE ${LIBNOVA_INCLUDE_DIR})

target_link_libraries(rts2-state PRIVATE base_kernel ${LIBNOVA_LIBRARY})
