cmake_minimum_required(VERSION 3.16)
project(base VERSION 0.1.0 LANGUAGES CXX C)

# base is a from-scratch CMake build of the RTS2 backend (wire protocol +
# device drivers, no database). It lives next to the autotools tree and is
# filled in piece by piece.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# For CMAKE_INSTALL_FULL_BINDIR/etc, used by packaging/ (rts2-start/stop,
# systemd units) below.
include(GNUInstallDirs)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wshadow)

set(BASE_CONFDIR "/etc" CACHE STRING "Base directory for rts2.ini and friends")

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/config/base-config.h.in
	${CMAKE_CURRENT_BINARY_DIR}/generated/base-config.h
)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/generated)

enable_testing()

# FLI (Finger Lakes Instrumentation) is a proprietary vendor SDK shared by
# both focusd/fli and filterd/fli - detected once here rather than
# duplicating the same find_path/find_library in both driver-family
# CMakeLists.txt files. Same opt-in reasoning as Andor/gxccd: nobody has
# this hardware/SDK by accident, so it's never auto-searched. Point
# BASE_FLI_SDK_DIR at a built libfli tree (containing libfli.h and
# libfli.a, both at the top level - that's how libfli's own Makefile lays
# it out, no separate include/ or lib/ subdirectory) to build the FLI
# drivers; leave it empty (the default) to skip them.
set(BASE_FLI_SDK_DIR "" CACHE PATH "Path to a built libfli tree (contains libfli.h and libfli.a at the top level); leave empty to skip building the FLI drivers")

if(BASE_FLI_SDK_DIR)
	find_path(FLI_INCLUDE_DIR NAMES libfli.h PATHS "${BASE_FLI_SDK_DIR}" NO_DEFAULT_PATH)
	find_library(FLI_LIBRARY NAMES fli PATHS "${BASE_FLI_SDK_DIR}" NO_DEFAULT_PATH)
	if(FLI_INCLUDE_DIR AND FLI_LIBRARY)
		message(STATUS "FLI SDK found (${FLI_INCLUDE_DIR}, ${FLI_LIBRARY}) - building FLI drivers")
	else()
		message(WARNING "BASE_FLI_SDK_DIR is set to '${BASE_FLI_SDK_DIR}' but libfli.h/libfli.a were not found there - skipping FLI drivers")
	endif()
else()
	message(STATUS "BASE_FLI_SDK_DIR not set - skipping FLI drivers (requires the proprietary FLI SDK)")
endif()

# libmks3 (Software Bisque Paramount ME/MYT MKS3 servo controller protocol)
# is Martin Jelinek's own C library (plain C source, not a prebuilt vendor
# .so like Andor/gxccd) - built from source like FLI, but here we only need
# the one file (libmks3.c/.h); the rest of that tree (drv.c/main.c/maps.c/
# tpmodel.c/simplc.c) belongs to a separate standalone "paracl" CLI tool
# and is not needed by rts2-teld-paramount. Same opt-in reasoning as
# Andor/gxccd/FLI: point BASE_PARACL_DIR at a checkout containing
# libmks3.c and libmks3.h to build rts2-teld-paramount; leave empty (the
# default) to skip it.
set(BASE_PARACL_DIR "" CACHE PATH "Path to Martin Jelinek's paracl tree (contains libmks3.c/libmks3.h); leave empty to skip building rts2-teld-paramount")

if(BASE_PARACL_DIR)
	find_file(PARACL_LIBMKS3_C NAMES libmks3.c PATHS "${BASE_PARACL_DIR}" NO_DEFAULT_PATH)
	find_file(PARACL_LIBMKS3_H NAMES libmks3.h PATHS "${BASE_PARACL_DIR}" NO_DEFAULT_PATH)
	if(PARACL_LIBMKS3_C AND PARACL_LIBMKS3_H)
		message(STATUS "paracl/libmks3 found (${PARACL_LIBMKS3_C}) - building rts2-teld-paramount")
	else()
		message(WARNING "BASE_PARACL_DIR is set to '${BASE_PARACL_DIR}' but libmks3.c/libmks3.h were not found there - skipping rts2-teld-paramount")
	endif()
else()
	message(STATUS "BASE_PARACL_DIR not set - skipping rts2-teld-paramount (requires Martin Jelinek's paracl/libmks3 source)")
endif()

add_subdirectory(kernel)

# Exported as a PUBLIC include on the target (not just the directory-scoped
# include_directories() above) so external projects that nest this tree via
# add_subdirectory() from a different directory scope - e.g. db - still
# pick up base-config.h transitively through target_link_libraries.
target_include_directories(base_kernel PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_subdirectory(centrald)
add_subdirectory(camd)
add_subdirectory(monitor)
add_subdirectory(teld)
add_subdirectory(focusd)
add_subdirectory(filterd)
add_subdirectory(sensord)
add_subdirectory(dome)
add_subdirectory(script)
add_subdirectory(scriptexec)
add_subdirectory(sendcmd)
add_subdirectory(focusc)

# Packaging assets: rts2-start/rts2-stop (the daemon-manifest-driven
# start/stop scripts, read @BASE_CONFDIR@/rts2/{centrald,devices,services}),
# systemd unit templates, and default /etc/rts2/ configuration - see
# packaging/README (if any) or base/STATUS.md's Debian-packaging section
# for the rationale. Not gated behind any option - useful for any install
# method, not just a .deb.
# Kept identical to the RTS2_CENTRALD_PORT/RTS2_LOCK_PREFIX baked into
# config/base-config.h.in - same values, just also needed as plain CMake
# variables here for substituting into shell scripts/systemd units
# (which can't #include a C header).
set(RTS2_CENTRALD_PORT "617")
set(RTS2_LOCK_PREFIX "/var/run/rts2_")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/packaging/rts2-start.in ${CMAKE_CURRENT_BINARY_DIR}/packaging/rts2-start @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/packaging/rts2-stop.in ${CMAKE_CURRENT_BINARY_DIR}/packaging/rts2-stop @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/packaging/rts2.service.in ${CMAKE_CURRENT_BINARY_DIR}/packaging/rts2.service @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/packaging/rts2@.service.in ${CMAKE_CURRENT_BINARY_DIR}/packaging/rts2@.service @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/packaging/rts2.ini ${CMAKE_CURRENT_BINARY_DIR}/packaging/rts2.ini @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/packaging/centrald.conf ${CMAKE_CURRENT_BINARY_DIR}/packaging/centrald @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/packaging/devices.conf ${CMAKE_CURRENT_BINARY_DIR}/packaging/devices @ONLY)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/packaging/services.conf ${CMAKE_CURRENT_BINARY_DIR}/packaging/services @ONLY)
file(COPY ${CMAKE_CURRENT_BINARY_DIR}/packaging/rts2-start ${CMAKE_CURRENT_BINARY_DIR}/packaging/rts2-stop
	DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/packaging
	FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
