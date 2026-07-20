db_ecpg_sources(RTS2EXECUTOR_ECPG_SOURCES
	src/rts2devcliphot.ec
)

add_library(db_executor STATIC
	${RTS2EXECUTOR_ECPG_SOURCES}
	src/executorque.cpp
	src/execclidb.cpp
)

target_include_directories(db_executor PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(db_executor PRIVATE
	${LIBPQ_INCLUDE_DIRS}
	${LIBECPG_INCLUDE_DIRS}
	${LIBECPG_COMPAT_INCLUDE_DIRS}
)

target_link_libraries(db_executor PUBLIC
	db
	base_script
	${LIBECPG_COMPAT_LIBRARIES}
	${LIBECPG_LIBRARIES}
	${LIBPQ_LIBRARIES}
)

add_executable(rts2-executor
	src/executor.cpp
)

target_link_libraries(rts2-executor PRIVATE db_executor)
