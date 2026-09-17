include("${PROJECT_SOURCE_DIR}/cmake/googletest.cmake")
bkmail_resolve_googletest_dependency()
include(GoogleTest)

function(bkmail_add_gtest target test_prefix)
  target_link_libraries(${target} PRIVATE GTest::gtest_main bkmail::bkmail)
  bkmail_set_project_warnings(${target})
  gtest_discover_tests(
    ${target}
    TEST_PREFIX "${test_prefix}."
    DISCOVERY_MODE PRE_TEST
    ${ARGN})
endfunction()

add_subdirectory(basic)
add_subdirectory(integration)
add_subdirectory(layer1)
add_subdirectory(layer2)
add_subdirectory(model)
add_subdirectory(proto)
