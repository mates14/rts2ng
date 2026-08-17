cmake_minimum_required(VERSION 3.16)
project(web VERSION 0.1.0 LANGUAGES CXX C)

# web is the HTTP/JSON API daemon (rts2-httpd) - a from-scratch redesign,
# not a mechanical port, of classic src/httpd. See STATUS.md for the full
# design writeup and rationale.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wshadow)

enable_testing()

# --- base (kernel + drivers), or db (rts2db + base nested) --------------
# STATUS.md task 7: WEB_WITH_DB picks which source tree gets nested via
# add_subdirectory - base directly (bus-only, tasks 1-6's build) or db
# (which nests base itself, adding rts2db - target/observation history
# endpoints). Never both: db already contains base, so nesting both would
# define the same library/executable targets twice and fail to configure.
# This is purely a compile-time/packaging choice (does the code and the
# libpq dependency exist in the binary at all) - see STATUS.md's "Runtime
# DB availability" section for the separate, narrower runtime question
# (DB reachable right now vs. not) that task 7 still has to handle even
# with WEB_WITH_DB ON.
option(WEB_WITH_DB "Build DB-bound endpoints (target/observation history) via ../db's rts2db - requires PostgreSQL" ON)

if(WEB_WITH_DB)
	set(WEB_DB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../db" CACHE PATH "Path to the db source tree (rts2db + base, nested) that web builds against when WEB_WITH_DB is ON")

	if(NOT EXISTS "${WEB_DB_DIR}/CMakeLists.txt")
		message(FATAL_ERROR "WEB_DB_DIR (${WEB_DB_DIR}) does not contain a CMakeLists.txt - not a db source tree")
	endif()

	add_subdirectory(${WEB_DB_DIR} db-build)
else()
	set(WEB_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../base" CACHE PATH "Path to the base source tree (kernel + drivers, no DB) that web builds against when WEB_WITH_DB is OFF")

	if(NOT EXISTS "${WEB_BASE_DIR}/CMakeLists.txt")
		message(FATAL_ERROR "WEB_BASE_DIR (${WEB_BASE_DIR}) does not contain a CMakeLists.txt - not a base source tree")
	endif()

	add_subdirectory(${WEB_BASE_DIR} base-build)
endif()

# --- libmicrohttpd -----------------------------------------------------
# HTTP serving substrate - see STATUS.md "Networking" design decision.
# Driven via its external-polling mode (MHD_USE_EPOLL, no internal
# thread), sharing Block's own poll() loop through one epoll fd
# (MHD_DAEMON_INFO_EPOLL_FD) rather than running a second event loop.
find_package(PkgConfig REQUIRED)
pkg_check_modules(MICROHTTPD REQUIRED libmicrohttpd)

# --- libjpeg -------------------------------------------------------------
# JPEG encoding for the /preview/ endpoint (STATUS.md task 3), replacing
# classic's Magick++ dependency - see STATUS.md's "Dropped outright".
pkg_check_modules(JPEG REQUIRED libjpeg)

# --- libcrypto -------------------------------------------------------------
# SHA-1 + base64 for the WebSocket handshake (STATUS.md task 4,
# Sec-WebSocket-Accept computation) - just the EVP digest/encode API, no
# TLS/libssl needed.
pkg_check_modules(OPENSSL_CRYPTO REQUIRED libcrypto)

# --- libcrypt --------------------------------------------------------------
# crypt(3)/crypt_r(3) for the local (non-DB) auth file's password hashes
# (STATUS.md task 6) - glibc split this out of libc itself some years
# back, now provided by libxcrypt.
pkg_check_modules(LIBCRYPT REQUIRED libcrypt)

add_subdirectory(httpd)
