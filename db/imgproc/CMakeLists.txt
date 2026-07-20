add_library(db_imgproc STATIC
	src/connimgprocess.cpp
)

target_include_directories(db_imgproc PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(db_imgproc PUBLIC
	db
)

add_executable(rts2-imgproc
	src/imgproc.cpp
)

target_link_libraries(rts2-imgproc PRIVATE db_imgproc)
