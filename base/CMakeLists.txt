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

# Which commit this build came from, so a log file can say what produced it.
# Two rounds of field testing were spent arguing about whether the binary at
# the telescope matched the source being discussed, and the honest answer was
# that nobody could tell from the log. Best effort only: not a git checkout,
# no git installed, or a source tarball all just give "unknown".
set(BASE_GIT_DESCRIBE "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
	execute_process(
		COMMAND ${GIT_EXECUTABLE} describe --always --dirty --tags
		WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
		OUTPUT_VARIABLE _base_git_describe
		OUTPUT_STRIP_TRAILING_WHITESPACE
		ERROR_QUIET
		RESULT_VARIABLE _base_git_result
	)
	if(_base_git_result EQUAL 0 AND _base_git_describe)
		set(BASE_GIT_DESCRIBE "${_base_git_describe}")
	endif()
endif()
message(STATUS "base: building from ${BASE_GIT_DESCRIBE}")

configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/config/base-config.h.in
	${CMAKE_CURRENT_BINARY_DIR}/generated/base-config.h
)
include_directories(${CMAKE_CURRENT_BINARY_DIR}/generated)

enable_testing()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/BaseOptions.cmake)

# FLI (Finger Lakes Instrumentation) is a proprietary vendor SDK shared by
# camd/fli, focusd/fli and filterd/fli - detected once here rather than
# duplicating the same find_path/find_library in each driver family.
# BASE_FLI_SDK_DIR points at a built libfli tree (libfli.h and libfli.a,
# both at the top level - that's how libfli's own Makefile lays it out, no
# separate include/ or lib/ subdirectory). buildme.sh proposes one; the
# families test BASE_HAVE_FLI rather than the cached find results, so
# BASE_FLI=OFF really turns the drivers off.
base_sdk_option(FLI BASE_FLI_SDK_DIR "FLI drivers (camd, focusd, filterd)" FLI_INCLUDE_DIR FLI_LIBRARY)
set(BASE_HAVE_FLI FALSE)
base_sdk_wanted(FLI BASE_FLI_SDK_DIR _want)
if(_want)
	find_path(FLI_INCLUDE_DIR NAMES libfli.h PATHS "${BASE_FLI_SDK_DIR}" NO_DEFAULT_PATH)
	find_library(FLI_LIBRARY NAMES fli PATHS "${BASE_FLI_SDK_DIR}" NO_DEFAULT_PATH)
	if(FLI_INCLUDE_DIR AND FLI_LIBRARY)
		set(BASE_HAVE_FLI TRUE)
	endif()
endif()
base_sdk_report(FLI BASE_FLI_SDK_DIR ${BASE_HAVE_FLI} "rts2-camd-fli, rts2-focusd-fli, rts2-filterd-fli" "libfli.h/libfli.a")

# libmks3 (Software Bisque Paramount ME/MYT MKS3 servo controller protocol)
# is Martin Jelinek's own C library (plain C source, not a prebuilt vendor
# .so like Andor/gxccd) - built from source like FLI, but here we only need
# the one file (libmks3.c/.h); the rest of that tree (drv.c/main.c/maps.c/
# tpmodel.c/simplc.c) belongs to a separate standalone "paracl" CLI tool
# and is not needed by rts2-teld-paramount. BASE_PARACL_DIR points at a
# checkout containing libmks3.c and libmks3.h.
base_sdk_option(PARACL BASE_PARACL_DIR "Paramount telescope driver (paracl/libmks3 source)" PARACL_LIBMKS3_C PARACL_LIBMKS3_H)
set(BASE_HAVE_PARACL FALSE)
base_sdk_wanted(PARACL BASE_PARACL_DIR _want)
if(_want)
	find_file(PARACL_LIBMKS3_C NAMES libmks3.c PATHS "${BASE_PARACL_DIR}" NO_DEFAULT_PATH)
	find_file(PARACL_LIBMKS3_H NAMES libmks3.h PATHS "${BASE_PARACL_DIR}" NO_DEFAULT_PATH)
	if(PARACL_LIBMKS3_C AND PARACL_LIBMKS3_H)
		set(BASE_HAVE_PARACL TRUE)
	endif()
endif()
base_sdk_report(PARACL BASE_PARACL_DIR ${BASE_HAVE_PARACL} "rts2-teld-paramount" "libmks3.c/libmks3.h")

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
