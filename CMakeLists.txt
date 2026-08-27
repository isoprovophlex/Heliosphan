cmake_minimum_required(VERSION 3.40)

set(NAME "Heliosphan" CACHE STRING "")
set(VERSION 0.1.0.0 CACHE STRING "")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

macro(set_from_environment VARIABLE)
  if(NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
    set(${VARIABLE} $ENV{${VARIABLE}})
  endif()
endmacro()

set_from_environment(VCPKG_ROOT)

if(DEFINED VCPKG_ROOT)
  set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
else()
  message(WARNING "Variable VCPKG_ROOT is not set. Continuing without vcpkg.")
endif()

project(
  ${NAME}
  VERSION ${VERSION}
  LANGUAGES CXX
)

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/CMake/Plugin.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/Plugin.h
  @ONLY
)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds are not allowed.")
endif()

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE tests" FORCE)
add_subdirectory(extern/CommonLibSSE)

include(CMake/headerlist.cmake)
include(CMake/sourcelist.cmake)

source_group(
  TREE ${CMAKE_CURRENT_SOURCE_DIR}
  FILES ${headers} ${sources}
)

add_library(
  ${PROJECT_NAME}
  SHARED
  ${headers}
  ${sources}
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_include_directories(
  ${PROJECT_NAME}
  PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/LumaUtil/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/MMSF/include
)

find_package(reflectcpp CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE
    CommonLibSSE::CommonLibSSE
    nlohmann_json::nlohmann_json
    reflectcpp::reflectcpp
    ZLIB::ZLIB
)

target_precompile_headers(${PROJECT_NAME} PRIVATE include/PCH.h)

if(MSVC)
  target_compile_options(
    ${PROJECT_NAME}
    PRIVATE
      /sdl
      /utf-8
      /Zi
      /MD
      /permissive-
      /Zc:preprocessor
      /wd4200
      "$<$<CONFIG:RELEASE>:/Zc:inline;/JMC-;/Ob3>"
  )

  target_link_options(
    ${PROJECT_NAME}
    PRIVATE
      "$<$<CONFIG:DEBUG>:/INCREMENTAL;/OPT:NOREF;/OPT:NOICF>"
      "$<$<CONFIG:RELEASE>:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF;/DEBUG:FULL>"
  )
endif()

option(HELIOSPHAN_BUILD_TESTS "Build Heliosphan development tests" OFF)

if(HELIOSPHAN_BUILD_TESTS)
  enable_testing()

  add_executable(
    HeliosphanTests
    tests/HeliosphanTests.cpp
    src/HeliosphanLogic.cpp
    src/PluginRecords.cpp
  )
  target_compile_features(HeliosphanTests PRIVATE cxx_std_23)
  target_include_directories(
    HeliosphanTests
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
  )
  target_link_libraries(
    HeliosphanTests
    PRIVATE
      nlohmann_json::nlohmann_json
  )
  target_compile_definitions(
    HeliosphanTests
    PRIVATE
      HELIOSPHAN_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
  )

  if(MSVC)
    target_compile_options(
      HeliosphanTests
      PRIVATE
        /utf-8
        /permissive-
        /Zc:preprocessor
    )
  endif()

  add_test(NAME HeliosphanTests COMMAND HeliosphanTests)
endif()
