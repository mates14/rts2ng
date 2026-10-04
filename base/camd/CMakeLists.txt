add_library(base_camd STATIC
	src/camd.cpp
	src/cliwheel.cpp
	src/clifocuser.cpp
)

target_include_directories(base_camd PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(base_camd PUBLIC base_kernel)

base_driver(dummy "Simulated camera")

# V4L2 webcam driver - needs only the Linux kernel UAPI header
# (linux/videodev2.h, no separate library), so unlike the vendor SDKs below
# there's nothing to detect: it always builds on Linux.
base_driver(v4l "V4L2 webcam")

# Andor is a proprietary vendor SDK (precompiled headers + .so, no source
# available). BASE_ANDOR_SDK_DIR points at an SDK tree (one containing
# include/atmcdLXd.h and lib/libandor-*.so), matching the classic tree's
# --with-andor=<path>; buildme.sh proposes one (see cmake/BaseOptions.cmake).
base_sdk_option(ANDOR BASE_ANDOR_SDK_DIR "Andor camera driver" ANDOR_INCLUDE_DIR)
set(_have_andor FALSE)
base_sdk_wanted(ANDOR BASE_ANDOR_SDK_DIR _want)
if(_want)
	find_path(ANDOR_INCLUDE_DIR NAMES atmcdLXd.h PATHS "${BASE_ANDOR_SDK_DIR}/include" NO_DEFAULT_PATH)
	# Vendor filename varies by SDK build variant - some ship
	# libandor-x86_64.so.<ver>, others libandor-stdc++6-x86_64.so.<ver> (an
	# extra tag between "libandor" and "x86_64") - match both, but not the
	# libandor-i386.so.<ver> 32-bit variant.
	file(GLOB ANDOR_LIBRARY_REAL "${BASE_ANDOR_SDK_DIR}/lib/libandor*x86_64.so*")
	if(ANDOR_INCLUDE_DIR AND ANDOR_LIBRARY_REAL)
		list(GET ANDOR_LIBRARY_REAL 0 ANDOR_LIBRARY_REAL)

		# The vendor .so's embedded SONAME is "libandor.so.2", but the SDK
		# tree ships only the fully-versioned filename (no unversioned
		# symlink - the vendor install script normally creates one under
		# /usr/lib, which we don't want to touch). Recreate that symlink
		# ourselves inside the build tree so the linker/runtime loader can
		# find it by SONAME without a system-wide install.
		set(ANDOR_LINK_DIR "${CMAKE_CURRENT_BINARY_DIR}/andor_lib")
		file(MAKE_DIRECTORY "${ANDOR_LINK_DIR}")
		file(REMOVE "${ANDOR_LINK_DIR}/libandor.so.2")
		file(CREATE_LINK "${ANDOR_LIBRARY_REAL}" "${ANDOR_LINK_DIR}/libandor.so.2" SYMBOLIC)
		set(ANDOR_LIBRARY "${ANDOR_LINK_DIR}/libandor.so.2")

		set(_have_andor TRUE)
		add_subdirectory(andor)
	endif()
endif()
base_sdk_report(ANDOR BASE_ANDOR_SDK_DIR ${_have_andor} "rts2-camd-andor" "include/atmcdLXd.h/lib/libandor*x86_64.so*")

# gxccd (Moravian Instruments). libgxccd is closed source, but its licence
# permits redistributing the unmodified binary, so the x86-64 static
# library is bundled in external/gxccd (see its README.md) and used
# whenever BASE_GXCCD_SDK_DIR is empty; the path overrides it, and is
# needed on other architectures (an SDK tree with include/gxccd.h and
# lib/libgxccd.a; classic tree: --with-gxccd=<path>).
#
# Only the static library is linked, even from an SDK tree that ships
# libgxccd.so next to it: plain find_library() would prefer the .so,
# making rts2-camd-gxccd need the vendor .so and an RPATH at runtime
# (base note, 2026-07-16). libusb-1.0 stays an ordinary dynamic system
# dependency.
base_sdk_option(GXCCD BASE_GXCCD_SDK_DIR "gxccd (Moravian Instruments) camera driver; empty = bundled external/gxccd (x86-64)" GXCCD_INCLUDE_DIR GXCCD_LIBRARY)
set(_have_gxccd FALSE)
set(_gxccd_bundled FALSE)
if(NOT BASE_GXCCD STREQUAL "OFF" AND "${BASE_GXCCD_SDK_DIR}" STREQUAL ""
		AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
	set(_gxccd_bundled TRUE)
	set(GXCCD_INCLUDE_DIR "${PROJECT_SOURCE_DIR}/external/gxccd/include")
	set(GXCCD_LIBRARY "${PROJECT_SOURCE_DIR}/external/gxccd/lib/libgxccd.a")
	set(_want TRUE)
else()
	base_sdk_wanted(GXCCD BASE_GXCCD_SDK_DIR _want)
	if(_want)
		find_path(GXCCD_INCLUDE_DIR NAMES gxccd.h PATHS "${BASE_GXCCD_SDK_DIR}/include" NO_DEFAULT_PATH)
		set(_gxccd_saved_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
		set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
		find_library(GXCCD_LIBRARY NAMES gxccd PATHS "${BASE_GXCCD_SDK_DIR}/lib" NO_DEFAULT_PATH)
		set(CMAKE_FIND_LIBRARY_SUFFIXES ${_gxccd_saved_suffixes})
	endif()
endif()
if(_want)
	find_package(PkgConfig)
	if(PkgConfig_FOUND)
		pkg_check_modules(LIBUSB1 libusb-1.0)
	endif()
	if(GXCCD_INCLUDE_DIR AND GXCCD_LIBRARY AND LIBUSB1_FOUND)
		set(_have_gxccd TRUE)
		add_subdirectory(gxccd)
	endif()
endif()
if(_gxccd_bundled)
	if(_have_gxccd)
		message(STATUS "GXCCD: using bundled external/gxccd - building rts2-camd-gxccd")
	elseif(BASE_GXCCD STREQUAL "ON")
		message(FATAL_ERROR "GXCCD: BASE_GXCCD=ON but system libusb-1.0 (pkg-config) not found")
	else()
		message(WARNING "GXCCD: system libusb-1.0 (pkg-config) not found - not building rts2-camd-gxccd")
	endif()
else()
	base_sdk_report(GXCCD BASE_GXCCD_SDK_DIR ${_have_gxccd} "rts2-camd-gxccd" "include/gxccd.h/lib/libgxccd.a (and system libusb-1.0)")
endif()

# FLI (Finger Lakes Instrumentation) camera driver. libfli setup
# (bundled external/fli, or BASE_FLI_SDK_DIR) happens once in the
# top-level base/CMakeLists.txt, shared with focusd/fli and filterd/fli
# (classic tree: --with-fli=<path>).
if(BASE_HAVE_FLI)
	add_subdirectory(fli)
endif()
