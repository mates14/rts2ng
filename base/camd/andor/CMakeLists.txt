add_executable(rts2-camd-andor andor.cpp)

target_include_directories(rts2-camd-andor PRIVATE ${ANDOR_INCLUDE_DIR})
target_link_libraries(rts2-camd-andor PRIVATE base_camd base_kernel ${ANDOR_LIBRARY})

# The vendor .so's embedded SONAME (libandor.so.2) isn't registered anywhere
# in the runtime linker path - the SDK is a private, non-system-installed
# copy (see camd/CMakeLists.txt). Bake its lib/ dir into RPATH so the
# binary finds it without needing LD_LIBRARY_PATH or a system-wide install.
get_filename_component(ANDOR_LIB_DIR "${ANDOR_LIBRARY}" DIRECTORY)
set_target_properties(rts2-camd-andor PROPERTIES
	BUILD_RPATH "${ANDOR_LIB_DIR}"
	INSTALL_RPATH "${ANDOR_LIB_DIR}"
)
