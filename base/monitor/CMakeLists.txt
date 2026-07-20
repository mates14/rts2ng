# ncurses ships pkg-config as "ncurses"/"ncursesw" on this system.
find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSES REQUIRED ncurses)

add_executable(rts2-mon
	src/nmonitor.cpp
	src/nwindow.cpp
	src/daemonwindow.cpp
	src/nmenu.cpp
	src/nmsgbox.cpp
	src/nmsgwindow.cpp
	src/nstatuswindow.cpp
	src/ncomwin.cpp
	src/nlayout.cpp
	src/nvaluebox.cpp
	src/ndevicewindow.cpp
	src/nwindowedit.cpp
)

target_include_directories(rts2-mon PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${NCURSES_INCLUDE_DIRS}
)

target_link_libraries(rts2-mon PRIVATE base_kernel ${NCURSES_LIBRARIES})
target_compile_options(rts2-mon PRIVATE ${NCURSES_CFLAGS_OTHER})
