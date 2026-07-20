add_executable(rts2-scriptexec
	src/scriptexec.cpp
)

target_include_directories(rts2-scriptexec PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rts2-scriptexec PRIVATE base_script)
