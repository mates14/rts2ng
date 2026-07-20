cmake_minimum_required(VERSION 3.16)
project(db VERSION 0.1.0 LANGUAGES CXX C)

# db is the database-bound half of the RTS2 backend split: the rts2db
# library (target/observation/plan persistence over PostgreSQL) and, on top
# of it, the rts2-executor observation-execution daemon. It depends on base
# (kernel + drivers, no DB - see ../base) but is otherwise its own project;
# see the design note referenced from base's STATUS.md for the reasoning.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_compile_options(-Wall -Wshadow)

enable_testing()

# --- base (kernel + drivers) --------------------------------------------
# db is built directly against an base source checkout (nested as a
# sub-project via add_subdirectory) rather than an installed package -
# base has no install/export step yet, and pinning to a source tree keeps
# this simple while both projects are still under active, single-tree
# development. Point DB_BASE_DIR elsewhere if base ever moves.
set(DB_BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../base" CACHE PATH "Path to the base source tree (kernel + drivers, no DB) that db builds against")

if(NOT EXISTS "${DB_BASE_DIR}/CMakeLists.txt")
	message(FATAL_ERROR "DB_BASE_DIR (${DB_BASE_DIR}) does not contain a CMakeLists.txt - not an base source tree")
endif()

add_subdirectory(${DB_BASE_DIR} base-build)

# --- PostgreSQL / ecpg ------------------------------------------------------
# rts2db is written against embedded SQL (.ec files, preprocessed by ecpg
# into plain C++). Unlike the vendor SDKs in base (Andor/gxccd/FLI/libmks3),
# this is a hard, unconditional dependency of db - there is no no-DB mode
# inside db itself (that's the whole point of the base/db split).
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPQ REQUIRED libpq)
pkg_check_modules(LIBECPG REQUIRED libecpg)
pkg_check_modules(LIBECPG_COMPAT REQUIRED libecpg_compat)
pkg_check_modules(LIBXML2 REQUIRED libxml-2.0)

find_program(ECPG_EXECUTABLE NAMES ecpg)
if(NOT ECPG_EXECUTABLE)
	message(FATAL_ERROR "ecpg preprocessor not found (part of the postgresql-server-dev / libecpg-dev package)")
endif()

# Preprocess each named .ec file (embedded-SQL C++, relative to
# CMAKE_CURRENT_SOURCE_DIR) into a plain .cpp via ecpg, generated into the
# matching binary dir. Appends the generated .cpp paths to OUTVAR in the
# caller's scope - pass that list straight to add_library/add_executable.
function(db_ecpg_sources OUTVAR)
	set(generated "")
	foreach(ec_file ${ARGN})
		get_filename_component(ec_name ${ec_file} NAME_WE)
		get_filename_component(ec_dir ${ec_file} DIRECTORY)
		set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/${ec_dir}")
		set(out_cpp "${out_dir}/${ec_name}.cpp")
		file(MAKE_DIRECTORY ${out_dir})
		add_custom_command(
			OUTPUT ${out_cpp}
			COMMAND ${ECPG_EXECUTABLE} -o ${out_cpp} ${CMAKE_CURRENT_SOURCE_DIR}/${ec_file}
			DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${ec_file}
			COMMENT "ecpg ${ec_file}"
			VERBATIM
		)
		list(APPEND generated ${out_cpp})
	endforeach()
	set(${OUTVAR} ${generated} PARENT_SCOPE)
endfunction()

add_subdirectory(db)
add_subdirectory(db/tools)
add_subdirectory(executor)
