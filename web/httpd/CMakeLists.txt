# STATUS.md task 5: worker thread pool (std::thread) for preview
# generation - not otherwise pulled in by base_kernel.
find_package(Threads REQUIRED)

add_executable(rts2-httpd src/httpd.cpp src/jsonvalue.cpp src/preview.cpp src/userauth.cpp src/websocket.cpp src/workerpool.cpp)

if(WEB_WITH_DB)
	# STATUS.md task 7: DB-bound endpoints. Linking against db alone is
	# enough - db's own CMakeLists.txt already links base_kernel PUBLIC,
	# so it comes along transitively; linking base_kernel again here too
	# would just be redundant, not wrong, but there's no reason to.
	target_sources(rts2-httpd PRIVATE src/dbendpoints.cpp)
	target_compile_definitions(rts2-httpd PRIVATE WEB_HAVE_DB)
	target_link_libraries(rts2-httpd PRIVATE db)
else()
	target_link_libraries(rts2-httpd PRIVATE base_kernel)
endif()

target_include_directories(rts2-httpd PRIVATE include ${MICROHTTPD_INCLUDE_DIRS} ${JPEG_INCLUDE_DIRS} ${OPENSSL_CRYPTO_INCLUDE_DIRS} ${LIBCRYPT_INCLUDE_DIRS})
target_compile_options(rts2-httpd PRIVATE ${MICROHTTPD_CFLAGS_OTHER} ${JPEG_CFLAGS_OTHER} ${OPENSSL_CRYPTO_CFLAGS_OTHER} ${LIBCRYPT_CFLAGS_OTHER})

target_link_libraries(rts2-httpd PRIVATE ${MICROHTTPD_LIBRARIES} ${JPEG_LIBRARIES} ${OPENSSL_CRYPTO_LIBRARIES} ${LIBCRYPT_LIBRARIES} Threads::Threads)
