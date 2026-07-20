# libnova ships no pkg-config file on this system - find it directly.
find_path(LIBNOVA_INCLUDE_DIR NAMES libnova/libnova.h)
find_library(LIBNOVA_LIBRARY NAMES nova)
if(NOT LIBNOVA_INCLUDE_DIR OR NOT LIBNOVA_LIBRARY)
	message(FATAL_ERROR "libnova development files not found")
endif()

# CFITSIO backs FitsFile/Image (real FITS writing, needed by execcli's
# scripted-observation image saving) - system package, found via pkg-config.
find_package(PkgConfig REQUIRED)
pkg_check_modules(CFITSIO REQUIRED cfitsio)

add_library(base_kernel STATIC
	src/hoststring.cpp
	src/radecparser.cpp
	src/utilsfunc.cpp
	src/riseset.cpp
	src/rts2format.cpp
	src/timestamp.cpp
	src/libnova_cpp.cpp
	src/message.cpp
	src/logstream.cpp
	src/option.cpp
	src/app.cpp
	src/cliapp.cpp
	src/askchoice.cpp
	src/pid.cpp
	src/objectcheck.cpp
	src/iniparser.cpp
	src/configuration.cpp
	src/centralstate.cpp
	src/value.cpp
	src/data.cpp
	src/valuestat.cpp
	src/valueminmax.cpp
	src/valuerectangle.cpp
	src/valuearray.cpp
	src/command.cpp
	src/connection.cpp
	src/networkaddress.cpp
	src/connuser.cpp
	src/devclient.cpp
	src/block.cpp
	src/daemon.cpp
	src/device.cpp
	src/multidev.cpp
	src/scriptdevice.cpp
	src/client.cpp
	src/displayvalue.cpp
	src/connnosend.cpp
	src/connudp.cpp
	src/connserial.cpp
	src/connethernet.cpp
	src/conntcp.cpp
	src/connfork.cpp
	src/connnotify.cpp
	src/connmodbus.cpp
	src/expander.cpp
	src/fitsfile.cpp
	src/channel.cpp
	src/rts2target.cpp
	src/image.cpp
	src/imageastrometry.cpp
	src/cameraimage.cpp
	src/devcliimg.cpp
	src/devclifoc.cpp
)

target_include_directories(base_kernel PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${LIBNOVA_INCLUDE_DIR}
	${CFITSIO_INCLUDE_DIRS}
)

target_link_libraries(base_kernel PUBLIC ${LIBNOVA_LIBRARY} ${CFITSIO_LIBRARIES})

add_subdirectory(tests)
