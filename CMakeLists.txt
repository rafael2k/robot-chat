cmake_minimum_required(VERSION 3.13)
project(robot-chat VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GNUTLS REQUIRED IMPORTED_TARGET gnutls)

# libirc: only the message parser and its helpers. The connection, queue and
# TLS parts of libirc are not used (see third_party/README.md).
add_library(ircparse STATIC
  third_party/libirc/lib/message.c
  third_party/libirc/lib/tag.c
  third_party/libirc/lib/strbuf.c
  third_party/libirc/lib/util.c)
target_include_directories(ircparse PUBLIC third_party/libirc)

add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cjson)

add_library(rccore STATIC
  src/util.c
  src/text.c
  src/conf.c
  src/net.c
  src/session.c
  src/http.c
  src/mcp.c)
target_include_directories(rccore PUBLIC src)
target_link_libraries(rccore PUBLIC ircparse cjson PkgConfig::GNUTLS)
target_compile_options(rccore PRIVATE -Wall -Wextra)

add_executable(robot-chat src/main.c)
target_link_libraries(robot-chat PRIVATE rccore)
target_compile_options(robot-chat PRIVATE -Wall -Wextra)
install(TARGETS robot-chat RUNTIME DESTINATION bin)

enable_testing()
foreach(t test_text test_parse)
  add_executable(${t} tests/${t}.c)
  target_link_libraries(${t} PRIVATE rccore)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
