db_ecpg_sources(RTS2DB_ECPG_SOURCES
	src/sqlerror.ec
	src/sqlcolumn.ec
	src/camlist.ec
	src/devicedb.ec
	src/appdb.ec
	src/labels.ec
	src/targetgrb.ec
	src/taruser.ec
	src/dbfilters.ec
	src/imagedb.ec
	src/imageset.ec
	src/observation.ec
	src/observationset.ec
	src/targetset.ec
	src/target.ec
	src/sub_targets.ec
	src/queues.ec
)

add_library(db STATIC
	${RTS2DB_ECPG_SOURCES}
	src/constraints.cpp
	src/targetell.cpp
	src/rts2targetplanet.cpp
	src/rts2count.cpp
	src/imagesetstat.cpp
	src/targetres.cpp
)

target_include_directories(db PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(db PRIVATE
	${LIBPQ_INCLUDE_DIRS}
	${LIBECPG_INCLUDE_DIRS}
	${LIBECPG_COMPAT_INCLUDE_DIRS}
)

# PUBLIC, not PRIVATE: constraints.h (a public header of this target) itself
# includes <libxml/parser.h>/<libxml/tree.h>, so anything linking db
# and including constraints.h transitively (e.g. db_executor) needs this
# propagated too.
target_include_directories(db PUBLIC
	${LIBXML2_INCLUDE_DIRS}
)

target_link_libraries(db PUBLIC
	base_kernel
	base_script
	${LIBECPG_COMPAT_LIBRARIES}
	${LIBECPG_LIBRARIES}
	${LIBPQ_LIBRARIES}
	${LIBXML2_LIBRARIES}
)
