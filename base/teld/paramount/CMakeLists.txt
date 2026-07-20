get_filename_component(PARACL_DIR "${PARACL_LIBMKS3_C}" DIRECTORY)

add_library(base_libmks3 STATIC
	${PARACL_LIBMKS3_C}
)
target_include_directories(base_libmks3 PUBLIC ${PARACL_DIR})
target_link_libraries(base_libmks3 PUBLIC m)

add_executable(rts2-teld-paramount paramount.cpp)
target_link_libraries(rts2-teld-paramount PRIVATE base_teld base_kernel base_libmks3)
