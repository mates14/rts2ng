add_library(base_script STATIC
	src/rts2spiral.cpp
	src/operands.cpp
	src/element.cpp
	src/elementblock.cpp
	src/elementtarget.cpp
	src/connexe.cpp
	src/elementexe.cpp
	src/elementhex.cpp
	src/elementwaitfor.cpp
	src/script.cpp
	src/devscript.cpp
	src/execcli.cpp
	src/scriptinterface.cpp
	src/scripttarget.cpp
)

target_include_directories(base_script PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(base_script PUBLIC base_kernel)
