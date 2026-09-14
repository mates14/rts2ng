# see kernel/tests/CMakeLists.txt for why -UNDEBUG is needed: these tests
# check with assert(), which the default RelWithDebInfo build would
# otherwise compile away along with the test.
add_executable(test_ra_distance test_ra_distance.cpp)
target_link_libraries(test_ra_distance PRIVATE base_teld)
target_compile_options(test_ra_distance PRIVATE -UNDEBUG)
add_test(NAME ra_distance COMMAND test_ra_distance)
