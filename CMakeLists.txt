cmake_minimum_required(VERSION 3.25)
project(StrataDB VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(STRATA_BUILD_TESTS "Build the GoogleTest suite" ON)
option(STRATA_BUILD_TOOLS "Build tools/*.cc (sst_dump, manifest_dump, ...)" ON)
option(STRATA_BUILD_BENCH "Build strata_bench benchmark harness" OFF)
option(STRATA_WERROR "Treat warnings as errors" OFF)
option(STRATA_DEBUG_CHECKS "Enable expensive invariant checks (STRATA_DCHECK)" OFF)
option(STRATA_CLANG_TIDY "Run clang-tidy on StrataDB sources while compiling" OFF)
set(STRATA_SANITIZER "" CACHE STRING
    "Sanitizers to enable: empty, 'address,undefined', or 'thread'")

# Sanitizers are applied globally (before GoogleTest is fetched) so that every
# object in the test binaries is instrumented. Mixing instrumented and
# uninstrumented code is the usual cause of TSan false positives.
if(STRATA_SANITIZER)
  if(STRATA_SANITIZER MATCHES "thread" AND STRATA_SANITIZER MATCHES "address")
    message(FATAL_ERROR "ThreadSanitizer cannot be combined with AddressSanitizer")
  endif()
  message(STATUS "StrataDB: sanitizers = ${STRATA_SANITIZER}")
  add_compile_options(-fsanitize=${STRATA_SANITIZER} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${STRATA_SANITIZER})
  if(STRATA_SANITIZER MATCHES "undefined")
    # Make UBSan findings fail the test instead of just printing.
    add_compile_options(-fno-sanitize-recover=all)
    add_link_options(-fno-sanitize-recover=all)
  endif()
endif()

# Warning flags for StrataDB's own targets only (not for GoogleTest).
add_library(strata_warnings INTERFACE)
target_compile_options(strata_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
  -Wcast-align -Woverloaded-virtual -Wnull-dereference -Wimplicit-fallthrough
  $<$<BOOL:${STRATA_WERROR}>:-Werror>)

if(STRATA_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
  set(STRATA_TIDY_COMMAND "${CLANG_TIDY_EXE}")
endif()

# Apply the project-wide settings to one of our own targets.
function(strata_configure_target target)
  target_link_libraries(${target} PRIVATE strata_warnings)
  if(STRATA_CLANG_TIDY)
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${STRATA_TIDY_COMMAND}")
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
# Sources are globbed so a new unit only needs its .cc file dropped into src/.
# CONFIGURE_DEPENDS makes the build re-glob when files are added or removed.
file(GLOB_RECURSE STRATA_SOURCES CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/src/*.cc")

add_library(strata STATIC ${STRATA_SOURCES})
add_library(strata::strata ALIAS strata)
target_include_directories(strata
  PUBLIC  "${PROJECT_SOURCE_DIR}/include"
  PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(strata PUBLIC
  STRATA_DEBUG_CHECKS=$<BOOL:${STRATA_DEBUG_CHECKS}>)
find_package(Threads REQUIRED)
target_link_libraries(strata PUBLIC Threads::Threads)
strata_configure_target(strata)

# ---------------------------------------------------------------------------
# Tools (one executable per tools/*.cc)
# ---------------------------------------------------------------------------
if(STRATA_BUILD_TOOLS)
  file(GLOB STRATA_TOOL_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/tools/*.cc")
  foreach(tool_src IN LISTS STRATA_TOOL_SOURCES)
    get_filename_component(tool_name "${tool_src}" NAME_WE)
    add_executable(${tool_name} "${tool_src}")
    target_include_directories(${tool_name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(${tool_name} PRIVATE strata)
    strata_configure_target(${tool_name})
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(STRATA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Benchmark Harness
# ---------------------------------------------------------------------------
if(STRATA_BUILD_BENCH)
  include(FetchContent)
  FetchContent_Declare(
    leveldb
    GIT_REPOSITORY https://github.com/google/leveldb.git
    GIT_TAG 1.23
    GIT_SHALLOW TRUE
  )
  set(LEVELDB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(LEVELDB_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(leveldb)

  # LevelDB 1.23 picks up our C++20 setting, and with assert() enabled it doesn't
  # compile as C++20. Treat its headers as system headers so our warning flags and
  # clang-tidy leave them alone.
  set_target_properties(leveldb PROPERTIES CXX_STANDARD 17)
  get_target_property(LEVELDB_INCLUDE_DIRS leveldb INTERFACE_INCLUDE_DIRECTORIES)
  set_target_properties(leveldb PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${LEVELDB_INCLUDE_DIRS}")

  file(GLOB STRATA_BENCH_SOURCES CONFIGURE_DEPENDS
       "${PROJECT_SOURCE_DIR}/bench/*.cc"
       "${PROJECT_SOURCE_DIR}/bench/adapters/*.cc")
  add_executable(strata_bench ${STRATA_BENCH_SOURCES})
  target_include_directories(strata_bench PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/bench"
    "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(strata_bench PRIVATE
    strata
    leveldb
    Threads::Threads)
  strata_configure_target(strata_bench)

  # LevelDB is built with -fno-rtti. This file derives from its Env classes, so it
  # needs the same flag, and UBSan's vptr check can't link against it.
  set_source_files_properties("${PROJECT_SOURCE_DIR}/bench/adapters/leveldb.cc"
    TARGET_DIRECTORY strata_bench
    PROPERTIES COMPILE_OPTIONS "-fno-rtti;-fno-sanitize=vptr")

  # Every run records the commit it measures and what LevelDB was built with: an
  # installed crc32c library, for one, gives LevelDB hardware checksums. Committing
  # updates .git/index, which re-runs configure and so refreshes the commit.
  set(STRATA_BENCH_COMMIT "unknown")
  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git/index")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${PROJECT_SOURCE_DIR}/.git/index")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
      WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
      OUTPUT_VARIABLE STRATA_BENCH_GIT_HEAD
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(STRATA_BENCH_GIT_HEAD)
      set(STRATA_BENCH_COMMIT "${STRATA_BENCH_GIT_HEAD}")
    endif()
  endif()
  set(STRATA_BENCH_LEVELDB_FEATURES "1.23")
  foreach(feature IN ITEMS CRC32C SNAPPY TCMALLOC)
    string(TOLOWER "${feature}" feature_name)
    if(HAVE_${feature})
      string(APPEND STRATA_BENCH_LEVELDB_FEATURES " ${feature_name}=yes")
    else()
      string(APPEND STRATA_BENCH_LEVELDB_FEATURES " ${feature_name}=no")
    endif()
  endforeach()
  target_compile_definitions(strata_bench PRIVATE
    "STRATA_BENCH_COMMIT=\"${STRATA_BENCH_COMMIT}\""
    "STRATA_BENCH_LEVELDB_FEATURES=\"${STRATA_BENCH_LEVELDB_FEATURES}\"")
endif()

# ---------------------------------------------------------------------------
# Formatting: `cmake --build <dir> --target format` / `format-check`
# ---------------------------------------------------------------------------
find_program(CLANG_FORMAT_EXE NAMES clang-format)
if(CLANG_FORMAT_EXE)
  file(GLOB_RECURSE STRATA_FORMAT_FILES CONFIGURE_DEPENDS
       "${PROJECT_SOURCE_DIR}/include/*.h"
       "${PROJECT_SOURCE_DIR}/src/*.h"   "${PROJECT_SOURCE_DIR}/src/*.cc"
       "${PROJECT_SOURCE_DIR}/tests/*.h" "${PROJECT_SOURCE_DIR}/tests/*.cc"
       "${PROJECT_SOURCE_DIR}/tools/*.h" "${PROJECT_SOURCE_DIR}/tools/*.cc"
       "${PROJECT_SOURCE_DIR}/bench/*.h" "${PROJECT_SOURCE_DIR}/bench/*.cc")
  add_custom_target(format
    COMMAND ${CLANG_FORMAT_EXE} -i ${STRATA_FORMAT_FILES}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Formatting sources with clang-format")
  add_custom_target(format-check
    COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${STRATA_FORMAT_FILES}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Checking formatting with clang-format")
endif()
