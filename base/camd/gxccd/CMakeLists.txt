add_executable(rts2-camd-gxccd gxccd.cpp)

target_include_directories(rts2-camd-gxccd PRIVATE ${GXCCD_INCLUDE_DIR} ${LIBUSB1_INCLUDE_DIRS})
target_link_libraries(rts2-camd-gxccd PRIVATE base_camd base_kernel ${GXCCD_LIBRARY} ${LIBUSB1_LIBRARIES})

# GXCCD_LIBRARY now resolves to the static libgxccd.a (see camd/CMakeLists.txt) -
# it's linked straight into the binary, so no RPATH/vendor .so to find at
# runtime. libusb-1.0 stays an ordinary dynamic system dependency,
# resolved via the normal linker search path.
