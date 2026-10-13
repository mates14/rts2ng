add_executable(rts2-dome-perek2m perek2m.cpp)
# the telescoped link (perek2m::Tcs) lives in base_teld, shared with rts2-teld-perek2m
target_link_libraries(rts2-dome-perek2m PRIVATE base_dome base_teld base_kernel)
