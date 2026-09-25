# rts2-stacker: a clone of rts2-viewer (../viewer) for demonstrating how a
# CCD image builds up - every frame is dark/flat-calibrated from a master
# calibration directory (--calib) and added into a running sum, shown live
# and saved on reset. The viewer itself is left alone; the display widgets
# it shares (image canvas, cursor item) are compiled straight from its
# source tree rather than copied.
set(CMAKE_AUTOMOC ON)

set(VIEWER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../viewer)

# Q_OBJECT headers listed as sources for AUTOMOC - see ../viewer/CMakeLists.txt.
add_executable(rts2-stacker
	src/main.cpp
	src/calibration.cpp include/stacker/calibration.h
	src/stackcamera.cpp include/stacker/stackcamera.h
	src/stackclient.cpp include/stacker/stackclient.h
	src/mainwindow.cpp include/stacker/mainwindow.h
	${VIEWER_DIR}/src/imagecanvas.cpp ${VIEWER_DIR}/include/gui/imagecanvas.h
	${VIEWER_DIR}/src/sightitem.cpp ${VIEWER_DIR}/include/gui/sightitem.h
	${VIEWER_DIR}/src/nightmode.cpp ${VIEWER_DIR}/include/gui/nightmode.h
)

target_include_directories(rts2-stacker PRIVATE include ${VIEWER_DIR}/include)
target_link_libraries(rts2-stacker PRIVATE base_kernel Qt5::Widgets)
