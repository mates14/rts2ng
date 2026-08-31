# No library of its own (unlike executor/imgproc): this daemon is one
# file, and everything it needs from the database side lives in db's own
# rts2db/records.h.
add_executable(rts2-recordd
	src/recordd.cpp
)

target_link_libraries(rts2-recordd PRIVATE db)
