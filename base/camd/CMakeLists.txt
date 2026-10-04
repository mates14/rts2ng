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

# Same opt-in as Andor above (classic tree: --with-gxccd=<path>).
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
base_sdk_option(GXCCD BASE_GXCCD_SDK_DIR "gxccd (Moravian Instruments) camera driver" GXCCD_INCLUDE_DIR GXCCD_LIBRARY)
set(_have_gxccd FALSE)
base_sdk_wanted(GXCCD BASE_GXCCD_SDK_DIR _want)
if(_want)
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
		set(_have_gxccd TRUE)
		add_subdirectory(gxccd)
	endif()
endif()
base_sdk_report(GXCCD BASE_GXCCD_SDK_DIR ${_have_gxccd} "rts2-camd-gxccd" "include/gxccd.h/lib/libgxccd.a (and system libusb-1.0)")

# FLI (Finger Lakes Instrumentation) camera driver. SDK detection
# (BASE_FLI_SDK_DIR -> FLI_INCLUDE_DIR/FLI_LIBRARY) happens once in the
# top-level base/CMakeLists.txt, shared with focusd/fli and filterd/fli
# (classic tree: --with-fli=<path>).
if(BASE_HAVE_FLI)
	add_subdirectory(fli)
endif()
