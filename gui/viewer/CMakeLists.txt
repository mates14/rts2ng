# Scoped to this directory only, so it doesn't leak AUTOMOC into the nested
# base-build subdirectory's plain-C++ targets (they don't use Qt/moc).
set(CMAKE_AUTOMOC ON)

# Headers containing Q_OBJECT are listed explicitly alongside their .cpp
# files, not just left to be found via #include: on this host's CMake
# (4.4.0, snap package), AUTOMOC's scan only picks up a Q_OBJECT header if
# it's a target source in its own right - a transitively #include'd header
# under a subdirectory include path (target_include_directories) is
# silently skipped (reproduced in isolation outside this project too).
add_executable(rts2-viewer
	src/main.cpp
	src/viewerclient.cpp include/gui/viewerclient.h
	src/viewercamera.cpp include/gui/viewercamera.h
	src/mainwindow.cpp include/gui/mainwindow.h
	src/imagecanvas.cpp include/gui/imagecanvas.h
	src/sightitem.cpp include/gui/sightitem.h
	src/focusgraphwidget.cpp include/gui/focusgraphwidget.h
)

target_include_directories(rts2-viewer PRIVATE include)
target_link_libraries(rts2-viewer PRIVATE base_kernel Qt5::Widgets)
