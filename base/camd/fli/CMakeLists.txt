add_executable(rts2-camd-fli fli.cpp)
target_include_directories(rts2-camd-fli PRIVATE ${FLI_INCLUDE_DIR})
target_link_libraries(rts2-camd-fli PRIVATE base_camd base_kernel ${FLI_LIBRARY})
