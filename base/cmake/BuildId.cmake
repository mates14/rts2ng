# Writes generated/base-build-id.h: the build ID that BUILD and the
# "build ..." log line report. Run by CMakeLists.txt at configure time and
# again by the base_build_id target on every build, so an incremental build
# after a pull reports the new commit, not the one cmake last configured
# from (the D50 Andor driver, 2026-11-15: built from 8d338b2, logged
# 8548677). configure_file () rewrites the header only when the ID changes,
# so an unchanged tree recompiles nothing. [JS 2026-11-15]
#
# In: BASE_SOURCE_DIR, BASE_BUILD_ID_IN, BASE_BUILD_ID_OUT.

set(BASE_GIT_DESCRIBE "unknown")
# buildme.sh -d passes the package version: it stamps debian/changelog
# before dpkg-buildpackage runs, which would make the describe below
# "-dirty" on every package build. [MJ 2026-11-03]
if(DEFINED ENV{RTS2_BUILD_ID} AND NOT "$ENV{RTS2_BUILD_ID}" STREQUAL "")
	set(BASE_GIT_DESCRIBE "$ENV{RTS2_BUILD_ID}")
else()
	# Best effort only: not a git checkout, no git installed, or a source
	# tarball all just give "unknown".
	find_package(Git QUIET)
	if(GIT_FOUND)
		execute_process(
			COMMAND ${GIT_EXECUTABLE} describe --always --dirty --tags
			WORKING_DIRECTORY ${BASE_SOURCE_DIR}
			OUTPUT_VARIABLE _base_git_describe
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET
			RESULT_VARIABLE _base_git_result
		)
		if(_base_git_result EQUAL 0 AND _base_git_describe)
			set(BASE_GIT_DESCRIBE "${_base_git_describe}")
		endif()
	endif()
endif()

configure_file(${BASE_BUILD_ID_IN} ${BASE_BUILD_ID_OUT} @ONLY)
