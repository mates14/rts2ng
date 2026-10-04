# Build-selection helpers - the "kernel-like configuration" of base. Every
# driver gets a cache option, so `ccmake build` (or cmake-gui) lists them
# all and lets one be switched off without editing any CMakeLists.txt.
# Options are named BASE_<FAMILY>_<DRIVER> (BASE_TELD_LX200,
# BASE_SENSORD_BART_RAIN, ...) so ccmake's alphabetical list groups them
# by family.
#
# Drivers needing a vendor SDK or out-of-tree source are not switched one
# by one but per SDK, with a three-state BASE_<SDK> option next to its
# BASE_<SDK>_SDK_DIR path (see base_sdk_option below). buildme.sh looks
# for the SDK, proposes a path and writes the confirmed one to the site
# file it passes in with `cmake -C`; CMake itself never searches, it only
# checks the path it is given.

# base_driver(<dir> <doc>)
#
# add_subdirectory(<dir>) behind BOOL option BASE_<FAMILY>_<DIR>, FAMILY
# being the name of the calling directory (camd, teld, ...). Default ON:
# drivers without external dependencies always build unless deselected.
function(base_driver dir doc)
	get_filename_component(_family "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
	string(TOUPPER "BASE_${_family}_${dir}" _opt)
	string(MAKE_C_IDENTIFIER "${_opt}" _opt)
	option(${_opt} "${doc}" ON)
	if(${_opt})
		add_subdirectory(${dir})
	endif()
endfunction()

# base_sdk_option(<NAME> <dirvar> <doc>)
#
# Declares the three-state BASE_<NAME> (AUTO/ON/OFF; Enter in ccmake
# cycles through them) and the path cache variable <dirvar>:
#   AUTO - build the drivers if <dirvar> is usable, skip them otherwise
#   ON   - <dirvar> must be usable, configure fails if it is not; used by
#          debian/rules so that a wrong path can no longer produce an
#          empty driver package with only a STATUS line to say why
#   OFF  - never build them, whatever <dirvar> says
#
# Changing <dirvar> (typically in ccmake) drops the cached find_* results
# listed in the remaining arguments - otherwise a header found under the
# old path would stay cached and the new path would be silently ignored.
function(base_sdk_option name dirvar doc)
	set(BASE_${name} AUTO CACHE STRING "${doc}: AUTO = build if ${dirvar} is usable, ON = fail if it is not, OFF = never build")
	set_property(CACHE BASE_${name} PROPERTY STRINGS AUTO ON OFF)
	if(NOT BASE_${name} MATCHES "^(AUTO|ON|OFF)$")
		message(FATAL_ERROR "BASE_${name} is '${BASE_${name}}', must be one of AUTO, ON, OFF")
	endif()
	set(${dirvar} "" CACHE PATH "${doc} - SDK location (buildme.sh proposes one); empty = none")
	if(NOT "${${dirvar}}" STREQUAL "${_BASE_LAST_${dirvar}}")
		foreach(_var IN LISTS ARGN)
			unset(${_var} CACHE)
		endforeach()
		set(_BASE_LAST_${dirvar} "${${dirvar}}" CACHE INTERNAL "")
	endif()
endfunction()

# base_sdk_wanted(<NAME> <dirvar> <outvar>)
#
# Sets <outvar> TRUE if detection should run at all: not OFF, and a path
# was given (an empty path under ON is reported by base_sdk_report).
function(base_sdk_wanted name dirvar outvar)
	if(NOT BASE_${name} STREQUAL "OFF" AND NOT "${${dirvar}}" STREQUAL "")
		set(${outvar} TRUE PARENT_SCOPE)
	else()
		set(${outvar} FALSE PARENT_SCOPE)
	endif()
endfunction()

# base_sdk_report(<NAME> <dirvar> <found> <what> <needs>)
#
# One message for the outcome of the detection: <what> is the list of
# drivers it gates, <needs> what was looked for under <dirvar>.
function(base_sdk_report name dirvar found what needs)
	if(found)
		message(STATUS "${name}: using ${${dirvar}} - building ${what}")
	elseif(BASE_${name} STREQUAL "OFF")
		message(STATUS "${name}: BASE_${name}=OFF - not building ${what}")
	elseif("${${dirvar}}" STREQUAL "")
		if(BASE_${name} STREQUAL "ON")
			message(FATAL_ERROR "${name}: BASE_${name}=ON but ${dirvar} is empty - set it to a tree containing ${needs} (buildme.sh -r asks for it)")
		endif()
		message(STATUS "${name}: ${dirvar} not set - not building ${what}")
	elseif(BASE_${name} STREQUAL "ON")
		message(FATAL_ERROR "${name}: BASE_${name}=ON but ${needs} not found under ${dirvar}=${${dirvar}}")
	else()
		message(WARNING "${name}: ${needs} not found under ${dirvar}=${${dirvar}} - not building ${what}")
	endif()
endfunction()
