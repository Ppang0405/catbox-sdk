cmake_minimum_required(VERSION 3.20)

# The version lives in one place, the public header.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/catbox.h" catbox_version_line REGEX "^#define CATBOX_VERSION ")
string(REGEX REPLACE "^#define CATBOX_VERSION \"([^\"]+)\".*" "\\1" catbox_version "${catbox_version_line}")

project(catbox
    VERSION ${catbox_version}
    DESCRIPTION "Upload files to catbox.moe and litterbox.catbox.moe"
    HOMEPAGE_URL "https://github.com/Ppang0405/catbox-sdk"
    LANGUAGES C)

option(BUILD_SHARED_LIBS "Build libcatbox as a shared library" OFF)
option(CATBOX_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(CATBOX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CATBOX_STATIC_CURL "Link libcurl and its dependencies statically (uses pkg-config --static)" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(GNUInstallDirs)

if(PROJECT_IS_TOP_LEVEL)
    # Executables and DLLs side by side, so Windows finds catbox.dll and the
    # libcurl DLLs that vcpkg copies next to each executable.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
endif()

if(CATBOX_STATIC_CURL)
    # FindCURL only knows libcurl itself; a static link also needs TLS, zlib,
    # nghttp2 and friends, which libcurl.pc lists under --static.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CURL_PC REQUIRED libcurl)
    add_library(CURL::libcurl INTERFACE IMPORTED)
    target_include_directories(CURL::libcurl INTERFACE ${CURL_PC_STATIC_INCLUDE_DIRS})
    target_compile_definitions(CURL::libcurl INTERFACE CURL_STATICLIB)
    target_link_libraries(CURL::libcurl INTERFACE ${CURL_PC_STATIC_LDFLAGS})
else()
    find_package(CURL REQUIRED)
endif()

function(catbox_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 $<$<BOOL:${CATBOX_WARNINGS_AS_ERRORS}>:/WX>)
        # fopen, getenv and strerror are fine here; the _s variants aren't portable.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic
            $<$<BOOL:${CATBOX_WARNINGS_AS_ERRORS}>:-Werror>)
    endif()
endfunction()

# ---- library -----------------------------------------------------------------

add_library(catbox src/catbox.c)
add_library(catbox::catbox ALIAS catbox)
target_include_directories(catbox PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(catbox PRIVATE CURL::libcurl)
set_target_properties(catbox PROPERTIES
    C_VISIBILITY_PRESET hidden
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    PUBLIC_HEADER include/catbox.h)
if(BUILD_SHARED_LIBS)
    target_compile_definitions(catbox PUBLIC CATBOX_SHARED PRIVATE CATBOX_BUILDING)
endif()
catbox_warnings(catbox)

# ---- command-line tool -------------------------------------------------------

add_executable(catbox_cli src/main.c src/cli.c src/progress.c)
# Named catbox like the library; a distinct PDB name keeps MSVC debug info for
# catbox.exe and a shared catbox.dll from overwriting each other.
set_target_properties(catbox_cli PROPERTIES OUTPUT_NAME catbox PDB_NAME catbox_cli)
target_link_libraries(catbox_cli PRIVATE catbox)
catbox_warnings(catbox_cli)

# ---- tests -------------------------------------------------------------------

if(CATBOX_BUILD_TESTS)
    enable_testing()

    # Compiles the sources directly (rather than linking the library) so the
    # tests can reach internals that a shared build would not export.
    add_executable(catbox_tests tests/test_catbox.c src/catbox.c src/cli.c src/progress.c)
    target_include_directories(catbox_tests PRIVATE include src)
    target_link_libraries(catbox_tests PRIVATE CURL::libcurl)
    catbox_warnings(catbox_tests)
    add_test(NAME unit COMMAND catbox_tests)

    add_test(NAME cli_version COMMAND catbox_cli --version)
    set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "catbox ${PROJECT_VERSION}")
    add_test(NAME cli_help COMMAND catbox_cli --help)
    set_tests_properties(cli_help PROPERTIES PASS_REGULAR_EXPRESSION "upload-url")
    add_test(NAME cli_usage_error COMMAND catbox_cli upload)
    set_tests_properties(cli_usage_error PROPERTIES WILL_FAIL TRUE)
endif()

# ---- install: headers, library, CLI, CMake package and pkg-config file ------

include(CMakePackageConfigHelpers)

install(TARGETS catbox EXPORT catboxTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS catbox_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

set(catbox_cmake_dir ${CMAKE_INSTALL_LIBDIR}/cmake/catbox)
install(EXPORT catboxTargets NAMESPACE catbox:: DESTINATION ${catbox_cmake_dir})
configure_package_config_file(cmake/catboxConfig.cmake.in
    "${PROJECT_BINARY_DIR}/catboxConfig.cmake"
    INSTALL_DESTINATION ${catbox_cmake_dir})
# 0.x releases may break the API between minor versions.
write_basic_package_version_file("${PROJECT_BINARY_DIR}/catboxConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion)
install(FILES
    "${PROJECT_BINARY_DIR}/catboxConfig.cmake"
    "${PROJECT_BINARY_DIR}/catboxConfigVersion.cmake"
    DESTINATION ${catbox_cmake_dir})

# A static libcatbox needs libcurl on every consumer's link line.
if(BUILD_SHARED_LIBS)
    set(catbox_pc_cflags "-DCATBOX_SHARED")
    set(catbox_pc_requires "Requires.private")
else()
    set(catbox_pc_requires "Requires")
endif()
configure_file(cmake/catbox.pc.in "${PROJECT_BINARY_DIR}/catbox.pc" @ONLY)
install(FILES "${PROJECT_BINARY_DIR}/catbox.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
