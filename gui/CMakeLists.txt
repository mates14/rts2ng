cmake_minimum_required(VERSION 3.16)
project(gui VERSION 0.1.0 LANGUAGES CXX C)

# gui is a Qt5 GUI camera-viewer client, sibling to base (kernel +
# drivers, no DB) and db (rts2db + rts2-executor). It replaces the old
# rts2-xfocusc tool: the RTS2 wire-protocol/image-receiving transport is
# reused from base's already-ported, focus-independent
# rts2image::DevClientCameraImage (kernel/include/devcliimg.h), and the
# raw-Xlib display (classic xfitsimage.cpp) is replaced by a Qt5
# QGraphicsView-based canvas modeled on the Perek-2m fiber pointing
# client's UI conventions (~/fiber_pointing/bin/fiber_pointing_client.py).
# See gui/STATUS.md for the design writeup.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wshadow)

enable_testing()

# --- base (kernel + drivers) --------------------------------------------
# Same nesting pattern as db/CMakeLists.txt: build straight against a
# source checkout (base has no install/export step yet), point
# GUI_BASE_DIR elsewhere if base ever moves.
set(GUI_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../base" CACHE PATH "Path to the base source tree (kernel + drivers, no DB) that gui builds against")

if(NOT EXISTS "${GUI_BASE_DIR}/CMakeLists.txt")
	message(FATAL_ERROR "GUI_BASE_DIR (${GUI_BASE_DIR}) does not contain a CMakeLists.txt - not an base source tree")
endif()

add_subdirectory(${GUI_BASE_DIR} base-build)

# --- Qt5 ------------------------------------------------------------------
# Qt5 is the entire point of this subtree (unlike the opt-in vendor SDKs in
# base - FLI/Andor/gxccd - which gate one driver among many), so it is a
# hard requirement here rather than soft-optional.
find_package(Qt5 5.12 COMPONENTS Widgets REQUIRED)

add_subdirectory(viewer)
