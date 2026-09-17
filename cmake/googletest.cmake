include_guard(GLOBAL)

set(BKMAIL_GOOGLETEST_PROVIDER
    "AUTO"
    CACHE STRING "How to resolve GoogleTest: AUTO, FIND_PACKAGE, or FETCH")
set_property(CACHE BKMAIL_GOOGLETEST_PROVIDER PROPERTY STRINGS AUTO
                                                       FIND_PACKAGE FETCH)
set(BKMAIL_GOOGLETEST_GIT_REPOSITORY
    "https://github.com/google/googletest.git"
    CACHE STRING "Git repository used by the GoogleTest FETCH provider")
set(BKMAIL_GOOGLETEST_GIT_TAG
    "v1.17.0"
    CACHE STRING "Git ref used by the GoogleTest FETCH provider")

function(bkmail_resolve_googletest_dependency)
  if(TARGET GTest::gtest_main)
    message(STATUS "Using the existing GTest::gtest_main target")
    return()
  endif()

  string(TOUPPER "${BKMAIL_GOOGLETEST_PROVIDER}"
                 _bkmail_googletest_provider)
  set(_bkmail_googletest_providers AUTO FIND_PACKAGE FETCH)
  if(NOT _bkmail_googletest_provider IN_LIST _bkmail_googletest_providers)
    message(
      FATAL_ERROR
        "BKMAIL_GOOGLETEST_PROVIDER must be AUTO, FIND_PACKAGE, or FETCH")
  endif()

  if(_bkmail_googletest_provider STREQUAL "AUTO")
    find_package(GTest CONFIG QUIET)
    if(NOT TARGET GTest::gtest_main)
      find_package(GTest QUIET)
    endif()

    if(TARGET GTest::gtest_main)
      set(_bkmail_googletest_provider FIND_PACKAGE)
    else()
      set(_bkmail_googletest_provider FETCH)
    endif()
  endif()

  if(_bkmail_googletest_provider STREQUAL "FIND_PACKAGE")
    if(NOT TARGET GTest::gtest_main)
      find_package(GTest CONFIG QUIET)
    endif()
    if(NOT TARGET GTest::gtest_main)
      find_package(GTest REQUIRED)
    endif()
    message(STATUS "Resolved GoogleTest with find_package")
  elseif(_bkmail_googletest_provider STREQUAL "FETCH")
    # GoogleTest is a test-only implementation detail of bkmail. Do not build
    # GoogleMock or add GoogleTest to bkmail's installation surface.
    set(BUILD_GMOCK OFF CACHE BOOL "Build GoogleMock" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "Install GoogleTest" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "Use the shared MSVC runtime"
                                        FORCE)

    include(FetchContent)
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY "${BKMAIL_GOOGLETEST_GIT_REPOSITORY}"
      GIT_TAG "${BKMAIL_GOOGLETEST_GIT_TAG}"
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(googletest)
    message(
      STATUS
        "Fetched GoogleTest ${BKMAIL_GOOGLETEST_GIT_TAG} from ${BKMAIL_GOOGLETEST_GIT_REPOSITORY}"
    )
  endif()

  if(NOT TARGET GTest::gtest_main)
    message(FATAL_ERROR
            "GoogleTest was resolved, but GTest::gtest_main was not found")
  endif()
endfunction()
