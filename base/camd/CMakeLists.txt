add_library(base_camd STATIC
	src/camd.cpp
)

target_include_directories(base_camd PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(base_camd PUBLIC base_kernel)

add_subdirectory(dummy)

# V4L2 webcam driver - needs only the Linux kernel UAPI header
# (linux/videodev2.h, no separate library), so unlike the vendor SDKs below
# there's nothing to detect: it always builds on Linux.
add_subdirectory(v4l)

# Andor is a proprietary vendor SDK (precompiled headers + .so, no source
# available) - nobody has this hardware/SDK by accident, so unlike libnova
# or ncurses this is never auto-searched. Point BASE_ANDOR_SDK_DIR at an
# SDK tree (one containing include/atmcdLXd.h and lib/libandor-*.so) to
# build rts2-camd-andor; leave it empty (the default) to skip the driver
# entirely, matching the classic tree's --with-andor=<path> opt-in.
set(BASE_ANDOR_SDK_DIR "" CACHE PATH "Path to an Andor SDK tree (contains include/ and lib/); leave empty to skip building rts2-camd-andor")

if(BASE_ANDOR_SDK_DIR)
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

		message(STATUS "Andor SDK found (${ANDOR_INCLUDE_DIR}, ${ANDOR_LIBRARY_REAL}) - building rts2-camd-andor")
		add_subdirectory(andor)
	else()
		message(WARNING "BASE_ANDOR_SDK_DIR is set to '${BASE_ANDOR_SDK_DIR}' but atmcdLXd.h/libandor-*-x86_64.so were not found there - skipping rts2-camd-andor")
	endif()
else()
	message(STATUS "BASE_ANDOR_SDK_DIR not set - skipping rts2-camd-andor (requires the proprietary Andor SDK)")
endif()

# Same opt-in reasoning as Andor above (classic tree: --with-gxccd=<path>).
# Unlike Andor's .so, this SDK ships a static libgxccd.a with a clean
# unversioned name - no SONAME/symlink wrinkle, so plain find_path/
# find_library is enough. Needs libusb-1.0 (has a working pkg-config file
# on this system, unlike libnova).
#
# base note (2026-07-16): this SDK tree ships *both* libgxccd.a and
# libgxccd.so side by side. Plain find_library() searches .so before .a
# (CMAKE_FIND_LIBRARY_SUFFIXES' default order), so despite the comment
# above this was actually resolving to the .so - meaning rts2-camd-gxccd
# needed the vendor .so + an RPATH at runtime, same wrinkle as Andor.
# Forcing .a here instead gives a genuinely static, portable binary (no
# externally-supplied vendor blob to ship alongside it) - motivated by
# wanting a "everything that can statically link its control library"
# base package. libgxccd.a's license explicitly permits binary
# redistribution, so this is a real static link, not just a build
# convenience. libusb-1.0 stays a normal dynamic system dependency
# either way (found below).
set(BASE_GXCCD_SDK_DIR "" CACHE PATH "Path to a gxccd (Moravian Instruments) SDK tree (contains include/ and lib/); leave empty to skip building rts2-camd-gxccd")

if(BASE_GXCCD_SDK_DIR)
	find_path(GXCCD_INCLUDE_DIR NAMES gxccd.h PATHS "${BASE_GXCCD_SDK_DIR}/include" NO_DEFAULT_PATH)
	set(_gxccd_saved_suffixes ${CMAKE_FIND_LIBRARY_SUFFIXES})
	set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
	find_library(GXCCD_LIBRARY NAMES gxccd PATHS "${BASE_GXCCD_SDK_DIR}/lib" NO_DEFAULT_PATH)
	set(CMAKE_FIND_LIBRARY_SUFFIXES ${_gxccd_saved_suffixes})
	find_package(PkgConfig)
	if(PkgConfig_FOUND)
		pkg_check_modules(LIBUSB1 libusb-1.0)
	endif()
	if(GXCCD_INCLUDE_DIR AND GXCCD_LIBRARY AND LIBUSB1_FOUND)
		message(STATUS "gxccd SDK found (${GXCCD_INCLUDE_DIR}, ${GXCCD_LIBRARY}) - building rts2-camd-gxccd")
		add_subdirectory(gxccd)
	else()
		message(WARNING "BASE_GXCCD_SDK_DIR is set to '${BASE_GXCCD_SDK_DIR}' but gxccd.h/libgxccd.a/libusb-1.0 were not all found - skipping rts2-camd-gxccd")
	endif()
else()
	message(STATUS "BASE_GXCCD_SDK_DIR not set - skipping rts2-camd-gxccd (requires the proprietary gxccd SDK)")
endif()

# FLI (Finger Lakes Instrumentation) camera driver. SDK detection
# (BASE_FLI_SDK_DIR -> FLI_INCLUDE_DIR/FLI_LIBRARY) happens once in the
# top-level base/CMakeLists.txt, shared with focusd/fli and filterd/fli -
# same opt-in reasoning as Andor/gxccd above (classic tree: --with-fli=<path>).
if(FLI_INCLUDE_DIR AND FLI_LIBRARY)
	add_subdirectory(fli)
endif()
