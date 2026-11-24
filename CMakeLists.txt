cmake_minimum_required(VERSION 3.20)
project(match3_strict VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(match3_strict_standalone OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(match3_strict_standalone ON)
endif()

option(MATCH3_STRICT_BUILD_CLI "Build the match3-solve command-line tool" ${match3_strict_standalone})
option(MATCH3_STRICT_BUILD_EXAMPLE "Build the public API example" ${match3_strict_standalone})
option(MATCH3_STRICT_BUILD_TESTS "Build the public API tests" ${match3_strict_standalone})

add_library(match3_strict STATIC src/solver.cpp)
add_library(match3_strict::match3_strict ALIAS match3_strict)
target_compile_features(match3_strict PUBLIC cxx_std_20)
target_include_directories(match3_strict PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(match3_strict PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON)

if(MATCH3_STRICT_BUILD_CLI)
    add_executable(match3-solve tools/solve.cpp)
    target_link_libraries(match3-solve PRIVATE match3_strict::match3_strict)
    install(TARGETS match3-solve RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(MATCH3_STRICT_BUILD_EXAMPLE)
    add_executable(match3-example examples/basic.cpp)
    target_link_libraries(match3-example PRIVATE match3_strict::match3_strict)
endif()

if(MATCH3_STRICT_BUILD_TESTS)
    enable_testing()
    add_executable(match3-strict-test tests/solver_test.cpp)
    target_link_libraries(match3-strict-test PRIVATE match3_strict::match3_strict)
    add_test(NAME strict_solver COMMAND match3-strict-test)
endif()

set(match3_strict_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/match3_strict")
configure_package_config_file(cmake/match3_strict-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/match3_strict-config.cmake"
    INSTALL_DESTINATION "${match3_strict_package_dir}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/match3_strict-config-version.cmake"
    VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion)

install(TARGETS match3_strict EXPORT match3_strict-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT match3_strict-targets NAMESPACE match3_strict:: DESTINATION "${match3_strict_package_dir}")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/match3_strict-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/match3_strict-config-version.cmake"
    DESTINATION "${match3_strict_package_dir}")
install(FILES LICENSE README.md README.zh-CN.md DESTINATION "${CMAKE_INSTALL_DATADIR}/match3_strict")
install(FILES examples/basic.cpp examples/board-7x9.txt DESTINATION "${CMAKE_INSTALL_DATADIR}/match3_strict/examples")
