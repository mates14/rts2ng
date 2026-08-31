# libcurl is checked for here rather than in web/CMakeLists.txt: it is a
# client-side dependency only - rts2-httpd itself serves HTTP with
# libmicrohttpd and never makes an outgoing request - so keeping the check
# in this subdirectory documents which binary actually needs it.
pkg_check_modules(CURL REQUIRED libcurl)

add_executable(rts2-jsonclient src/jsonclient.cpp src/httpfetch.cpp src/json.cpp)

# base_kernel only (never db, even when WEB_WITH_DB is ON): this client
# talks to the daemon over HTTP and has no database connection of its own -
# the /api/db/ endpoints are the daemon's business. What it uses from the
# kernel is the CliApp option/help/askForPassword scaffolding every other
# RTS2 command line tool uses, plus Timestamp and message.h's severity bits.
target_link_libraries(rts2-jsonclient PRIVATE base_kernel)

target_include_directories(rts2-jsonclient PRIVATE include ${CURL_INCLUDE_DIRS})
target_compile_options(rts2-jsonclient PRIVATE ${CURL_CFLAGS_OTHER})
target_link_libraries(rts2-jsonclient PRIVATE ${CURL_LIBRARIES})
