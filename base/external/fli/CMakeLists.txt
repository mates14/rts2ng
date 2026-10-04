# libfli built from the bundled FLI SDK (libfli/, FLI SDK 1.104 + RTS2
# patches, see README.md) - the same objects libfli's own Makefile puts
# into libfli.a on Linux. Static, like the external libfli.a it replaces.
# The vendor tree itself is kept as imported; everything this build needs
# to know about it is here.
set(_libfli ${CMAKE_CURRENT_SOURCE_DIR}/libfli)

add_library(base_libfli STATIC
	${_libfli}/libfli.c
	${_libfli}/libfli-camera.c
	${_libfli}/libfli-camera-parport.c
	${_libfli}/libfli-camera-usb.c
	${_libfli}/libfli-filter-focuser.c
	${_libfli}/libfli-mem.c
	${_libfli}/unix/libfli-debug.c
	${_libfli}/unix/libfli-serial.c
	${_libfli}/unix/libfli-sys.c
	${_libfli}/unix/libfli-usb.c
	${_libfli}/unix/linux/libfli-parport.c
	${_libfli}/unix/linux/libfli-usb-sys.c
)
target_include_directories(base_libfli PUBLIC ${_libfli} PRIVATE ${_libfli}/unix)
set_target_properties(base_libfli PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Vendor code: its warnings are not ours to fix, and base's -Wall -Wshadow
# would bury the build log in them.
target_compile_options(base_libfli PRIVATE -w)
