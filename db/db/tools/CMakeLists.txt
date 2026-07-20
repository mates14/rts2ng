# Target-management CLI tools - the minimal set needed to create/inspect/
# edit targets so they can be fed to rts2-executor. See db/STATUS.md for
# what's ported here vs. deliberately left out (rts2-obsinfo, rts2-plan,
# rts2-simbadinfo, rts2-nightreport, rts2-airmasscale, rts2-tpm, rts2-user*,
# rts2-horizon, rts2-image - none of them needed to feed the executor a
# target).
#
# printtarget.{h,cpp} and rts2targetapp.{h,cpp} are shared helpers used by
# only one tool each in this reduced scope, so they're compiled straight
# into their one consumer rather than promoted into a library.

add_executable(rts2-newtarget
	newtarget.cpp
	rts2targetapp.cpp
)
target_link_libraries(rts2-newtarget PRIVATE db base_script)

add_executable(rts2-target
	target.cpp
)
target_link_libraries(rts2-target PRIVATE db base_script)

add_executable(rts2-targetinfo
	targetinfo.cpp
	printtarget.cpp
)
target_link_libraries(rts2-targetinfo PRIVATE db base_script)

add_executable(rts2-targetlist
	targetlist.cpp
)
target_link_libraries(rts2-targetlist PRIVATE db base_script)
