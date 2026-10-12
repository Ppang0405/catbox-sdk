cmake_minimum_required(VERSION 3.20)

project(catbox VERSION 0.1.0 DESCRIPTION "catbox.moe and litterbox clients" LANGUAGES CXX)

# Build the CLI and tests only when this is the top-level project, not when another project
# pulls the library in with add_subdirectory or FetchContent.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(catbox_is_top_level ON)
else()
  set(catbox_is_top_level OFF)
endif()
option(CATBOX_BUILD_CLI "Build the catbox command-line tool" ${catbox_is_top_level})
option(CATBOX_BUILD_TESTS "Build the unit tests" ${catbox_is_top_level})
option(CATBOX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

find_package(CURL REQUIRED)

# Warning flags shared by every target in this project (but not by consumers of the library).
add_library(catbox_warnings INTERFACE)
if(MSVC)
  target_compile_options(catbox_warnings INTERFACE /W4 /permissive- /utf-8)
  if(CATBOX_WARNINGS_AS_ERRORS)
    target_compile_options(catbox_warnings INTERFACE /WX)
  endif()
else()
  target_compile_options(catbox_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
  if(CATBOX_WARNINGS_AS_ERRORS)
    target_compile_options(catbox_warnings INTERFACE -Werror)
  endif()
endif()

# The library: catbox::catbox
add_library(catbox STATIC
  src/client.cpp
  src/mime.cpp
  src/request.cpp
)
add_library(catbox::catbox ALIAS catbox)
target_compile_features(catbox PUBLIC cxx_std_20)
target_include_directories(catbox PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(catbox PRIVATE CURL::libcurl $<BUILD_INTERFACE:catbox_warnings>)

if(CATBOX_BUILD_CLI)
  include(FetchContent)
  FetchContent_Declare(cli11
    URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.7.2.tar.gz
    URL_HASH SHA256=46eef3101da70852ec7af026e09d485ccee81813331c8c6052d39344443b83da
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(cli11)

  add_executable(catbox_cli cli/main.cpp cli/terminal.cpp)
  set_target_properties(catbox_cli PROPERTIES OUTPUT_NAME catbox)
  target_link_libraries(catbox_cli PRIVATE catbox::catbox CLI11::CLI11 catbox_warnings)
  install(TARGETS catbox_cli RUNTIME DESTINATION bin)
endif()

if(CATBOX_BUILD_TESTS)
  enable_testing()
  add_executable(catbox_tests tests/catbox_tests.cpp)
  # The tests also exercise internal helpers such as detail::parse_response.
  target_include_directories(catbox_tests PRIVATE src)
  target_link_libraries(catbox_tests PRIVATE catbox::catbox CURL::libcurl catbox_warnings)
  add_test(NAME catbox_tests COMMAND catbox_tests)
endif()
