include(FetchContent)

# GoogleTest is built from source so it picks up the sanitizer flags above.
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
  GIT_SHALLOW    TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# Long-running suites get the "slow" label; everything else is "unit".
#   ctest -L unit      fast feedback while developing
#   ctest -L slow      model / crash / kill tests
set(STRATA_SLOW_TESTS model_test crash_test kill_test)

# Every tests/*_test.cc becomes its own executable, linked against the library.
file(GLOB STRATA_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cc")
foreach(test_src IN LISTS STRATA_TEST_SOURCES)
  get_filename_component(test_name "${test_src}" NAME_WE)
  add_executable(${test_name} "${test_src}")
  target_include_directories(${test_name} PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(${test_name} PRIVATE strata GTest::gtest_main)
  strata_configure_target(${test_name})

  if(test_name IN_LIST STRATA_SLOW_TESTS)
    set(test_label slow)
  else()
    set(test_label unit)
  endif()
  # PRE_TEST: list the tests when ctest runs, not at build time, so a
  # sanitizer that aborts at startup shows up as a test failure.
  gtest_discover_tests(${test_name}
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS ${test_label})
endforeach()
