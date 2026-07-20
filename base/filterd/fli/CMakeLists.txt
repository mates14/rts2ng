add_executable(rts2-filterd-fli fli.cpp)
target_include_directories(rts2-filterd-fli PRIVATE ${FLI_INCLUDE_DIR})
target_link_libraries(rts2-filterd-fli PRIVATE base_filterd base_kernel ${FLI_LIBRARY})
