cmake_minimum_required(VERSION 3.16)

project(TFDB VERSION 1.1.0 LANGUAGES CXX)

# PROJECT_IS_TOP_LEVEL exists only from CMake 3.21; keep the 3.16 floor usable.
if(DEFINED PROJECT_IS_TOP_LEVEL)
  set(TFDB_IS_TOP_LEVEL ${PROJECT_IS_TOP_LEVEL})
elseif(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(TFDB_IS_TOP_LEVEL ON)
else()
  set(TFDB_IS_TOP_LEVEL OFF)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Tools, examples, tests, and install rules are for people building TFDB
# itself. A parent project that pulls TFDB in with add_subdirectory() or
# FetchContent gets the library only, and does not inherit a BUILD_TESTING
# option or TFDB install rules it never asked for.
option(TFDB_BUILD_TOOLS "Build TFDB command-line tools" ${TFDB_IS_TOP_LEVEL})
option(TFDB_BUILD_EXAMPLES "Build TFDB examples" ${TFDB_IS_TOP_LEVEL})
option(TFDB_BUILD_TESTING "Build TFDB tests" ${TFDB_IS_TOP_LEVEL})
option(TFDB_INSTALL "Generate TFDB install and package rules" ${TFDB_IS_TOP_LEVEL})

# A static library linked into a shared object needs -fPIC. Default to whatever
# the consumer already asked for globally.
set(TFDB_PIC_DEFAULT OFF)
if(CMAKE_POSITION_INDEPENDENT_CODE)
  set(TFDB_PIC_DEFAULT ON)
endif()
option(TFDB_POSITION_INDEPENDENT_CODE
       "Build libtfdb with -fPIC so it can be linked into a shared library"
       ${TFDB_PIC_DEFAULT})

if(TFDB_BUILD_TESTING)
  include(CTest)
endif()

find_package(Threads REQUIRED)

set(TFDB_SOURCES
    src/status.cpp
    src/version.cpp
    src/storage_posix.cpp
    src/memory_storage.cpp
    src/counting_storage.cpp
    src/codec.cpp
    src/internal_format.cpp
    src/ring_store.cpp
    src/record_profile.cpp
    src/async_writer.cpp)

add_library(tfdb STATIC ${TFDB_SOURCES})
add_library(TFDB::tfdb ALIAS tfdb)
target_compile_features(tfdb PUBLIC cxx_std_14)
set_target_properties(tfdb PROPERTIES
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ${TFDB_POSITION_INDEPENDENT_CODE})
target_include_directories(tfdb
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE src)
target_link_libraries(tfdb PUBLIC Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(tfdb PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2)
endif()

if(TFDB_BUILD_TOOLS)
  foreach(tool IN ITEMS format inspect verify dump loadgen)
    add_executable(tfdb_${tool} tools/tfdb_${tool}.cpp)
    target_include_directories(tfdb_${tool} PRIVATE tools src)
    target_link_libraries(tfdb_${tool} PRIVATE TFDB::tfdb)
  endforeach()
endif()

if(TFDB_BUILD_EXAMPLES)
  add_executable(tfdb_example_quickstart examples/quickstart.cpp)
  target_link_libraries(tfdb_example_quickstart PRIVATE TFDB::tfdb)
  add_executable(tfdb_example_async_writer examples/async_writer.cpp)
  target_link_libraries(tfdb_example_async_writer PRIVATE TFDB::tfdb)
endif()

if(TFDB_BUILD_TESTING AND BUILD_TESTING)
  add_executable(tfdb_tests tests/test_main.cpp)
  target_include_directories(tfdb_tests PRIVATE src)
  target_link_libraries(tfdb_tests PRIVATE TFDB::tfdb)
  add_test(NAME tfdb_unit COMMAND tfdb_tests)

  add_executable(tfdb_make_conformance_volume tests/make_conformance_volume.cpp)
  target_link_libraries(tfdb_make_conformance_volume PRIVATE TFDB::tfdb)

  add_executable(tfdb_make_conformance_cases tests/make_conformance_cases.cpp)
  target_link_libraries(tfdb_make_conformance_cases PRIVATE TFDB::tfdb)

  if(TFDB_BUILD_TOOLS)
    find_program(TFDB_BASH_PROGRAM bash)
    if(NOT TFDB_BASH_PROGRAM)
      message(FATAL_ERROR "bash is required for the TFDB tool smoke test")
    endif()
    add_test(NAME tfdb_tool_smoke
             COMMAND ${TFDB_BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tool_smoke.sh)
    set_tests_properties(tfdb_tool_smoke PROPERTIES
        ENVIRONMENT "TOOL_DIR=$<TARGET_FILE_DIR:tfdb_format>")
  endif()
endif()

if(TFDB_INSTALL)
  install(TARGETS tfdb
      EXPORT TFDBTargets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(DIRECTORY include/tfdb DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  install(FILES LICENSE CHANGELOG.md
      DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/tfdb)

  if(TFDB_BUILD_TOOLS)
    install(TARGETS
        tfdb_format tfdb_inspect tfdb_verify tfdb_dump tfdb_loadgen
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()

  write_basic_package_version_file(
      ${CMAKE_CURRENT_BINARY_DIR}/TFDBConfigVersion.cmake
      VERSION ${PROJECT_VERSION}
      COMPATIBILITY SameMajorVersion)
  configure_package_config_file(
      cmake/TFDBConfig.cmake.in
      ${CMAKE_CURRENT_BINARY_DIR}/TFDBConfig.cmake
      INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TFDB)
  install(FILES
      ${CMAKE_CURRENT_BINARY_DIR}/TFDBConfig.cmake
      ${CMAKE_CURRENT_BINARY_DIR}/TFDBConfigVersion.cmake
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TFDB)
  install(EXPORT TFDBTargets
      FILE TFDBTargets.cmake
      NAMESPACE TFDB::
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/TFDB)
endif()
