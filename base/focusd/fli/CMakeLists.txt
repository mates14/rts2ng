add_executable(rts2-focusd-fli fli.cpp)
target_include_directories(rts2-focusd-fli PRIVATE ${FLI_INCLUDE_DIR})
target_link_libraries(rts2-focusd-fli PRIVATE base_focusd base_kernel ${FLI_LIBRARY})
