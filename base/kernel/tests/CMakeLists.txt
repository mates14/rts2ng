add_executable(test_hoststring test_hoststring.cpp)
target_link_libraries(test_hoststring PRIVATE base_kernel)
add_test(NAME hoststring COMMAND test_hoststring)

add_executable(test_libnova_cpp test_libnova_cpp.cpp)
target_link_libraries(test_libnova_cpp PRIVATE base_kernel)
add_test(NAME libnova_cpp COMMAND test_libnova_cpp)

add_executable(test_config test_config.cpp)
target_link_libraries(test_config PRIVATE base_kernel)
add_test(NAME config COMMAND test_config)

add_executable(test_value_header test_value_header.cpp)
target_link_libraries(test_value_header PRIVATE base_kernel)
add_test(NAME value_header COMMAND test_value_header)

add_executable(test_connection_prereqs test_connection_prereqs.cpp)
target_link_libraries(test_connection_prereqs PRIVATE base_kernel)
add_test(NAME connection_prereqs COMMAND test_connection_prereqs)

add_executable(test_app test_app.cpp)
target_link_libraries(test_app PRIVATE base_kernel)
add_test(NAME app COMMAND test_app)

add_executable(test_block_connection test_block_connection.cpp)
target_link_libraries(test_block_connection PRIVATE base_kernel)
add_test(NAME block_connection COMMAND test_block_connection)
