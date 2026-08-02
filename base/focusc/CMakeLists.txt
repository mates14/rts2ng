# ncurses ships pkg-config as "ncurses"/"ncursesw" on this system.
find_package(PkgConfig REQUIRED)
pkg_check_modules(NCURSES REQUIRED ncurses)

add_executable(rts2-focusc
	src/focusclient.cpp
	src/focusc.cpp
)

target_include_directories(rts2-focusc PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/include
	${NCURSES_INCLUDE_DIRS}
)

target_link_libraries(rts2-focusc PRIVATE base_kernel ${NCURSES_LIBRARIES})
target_compile_options(rts2-focusc PRIVATE ${NCURSES_CFLAGS_OTHER})
