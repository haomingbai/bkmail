include_guard(GLOBAL)

set(BKMAIL_BEXEC_PROVIDER
    "AUTO"
    CACHE STRING
          "How to resolve bexec: AUTO, FIND_PACKAGE, SOURCE, or FETCH")
set_property(CACHE BKMAIL_BEXEC_PROVIDER PROPERTY STRINGS AUTO FIND_PACKAGE
                                                 SOURCE FETCH)
set(BKMAIL_BEXEC_MIN_VERSION
    "0.1.0"
    CACHE STRING "Minimum accepted bexec package version")
set(BKMAIL_BEXEC_SOURCE_DIR
    ""
    CACHE PATH "Path to a local bexec source checkout")
set(BKMAIL_BEXEC_GIT_REPOSITORY
    "https://github.com/haomingbai/bexec.git"
    CACHE STRING "Git repository used by the FETCH provider")
set(BKMAIL_BEXEC_GIT_TAG
    "v0.1.0"
    CACHE STRING "Git ref used by the FETCH provider")

function(bkmail_resolve_bexec_dependency)
  if(TARGET bexec::bexec)
    message(STATUS "Using the existing bexec::bexec target")
    return()
  endif()

  if(TARGET bexec)
    add_library(bexec::bexec ALIAS bexec)
    message(STATUS "Using the existing bexec target")
    return()
  endif()

  string(TOUPPER "${BKMAIL_BEXEC_PROVIDER}" _bkmail_bexec_provider)
  set(_bkmail_bexec_providers AUTO FIND_PACKAGE SOURCE FETCH)
  if(NOT _bkmail_bexec_provider IN_LIST _bkmail_bexec_providers)
    message(
      FATAL_ERROR
        "BKMAIL_BEXEC_PROVIDER must be AUTO, FIND_PACKAGE, SOURCE, or FETCH")
  endif()

  if(_bkmail_bexec_provider STREQUAL "AUTO")
    if(BKMAIL_BEXEC_SOURCE_DIR)
      set(_bkmail_bexec_provider SOURCE)
    else()
      find_package(bexec ${BKMAIL_BEXEC_MIN_VERSION} CONFIG QUIET)
      if(TARGET bexec::bexec OR TARGET bexec)
        set(_bkmail_bexec_provider FIND_PACKAGE)
      else()
        set(_bkmail_bexec_provider FETCH)
      endif()
    endif()
  endif()

  if(_bkmail_bexec_provider STREQUAL "FIND_PACKAGE")
    if(NOT TARGET bexec::bexec AND NOT TARGET bexec)
      find_package(bexec ${BKMAIL_BEXEC_MIN_VERSION} CONFIG REQUIRED)
    endif()
    message(STATUS "Resolved bexec with find_package")
  elseif(_bkmail_bexec_provider STREQUAL "SOURCE")
    if(NOT BKMAIL_BEXEC_SOURCE_DIR)
      message(
        FATAL_ERROR
          "BKMAIL_BEXEC_SOURCE_DIR is required when BKMAIL_BEXEC_PROVIDER=SOURCE")
    endif()
    get_filename_component(_bkmail_bexec_source_dir "${BKMAIL_BEXEC_SOURCE_DIR}"
                           ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    if(NOT EXISTS "${_bkmail_bexec_source_dir}/CMakeLists.txt")
      message(
        FATAL_ERROR
          "BKMAIL_BEXEC_SOURCE_DIR must point to a bexec source tree")
    endif()

    set(BEXEC_BUILD_TESTS OFF CACHE BOOL "Build bexec tests" FORCE)
    set(BEXEC_BUILD_EXAMPLES OFF CACHE BOOL "Build bexec examples" FORCE)
    add_subdirectory("${_bkmail_bexec_source_dir}"
                     "${PROJECT_BINARY_DIR}/_deps/bexec-build"
                     EXCLUDE_FROM_ALL)
    message(STATUS "Resolved bexec from ${_bkmail_bexec_source_dir}")
  elseif(_bkmail_bexec_provider STREQUAL "FETCH")
    set(BEXEC_BUILD_TESTS OFF CACHE BOOL "Build bexec tests" FORCE)
    set(BEXEC_BUILD_EXAMPLES OFF CACHE BOOL "Build bexec examples" FORCE)
    include(FetchContent)
    FetchContent_Declare(
      bexec
      GIT_REPOSITORY "${BKMAIL_BEXEC_GIT_REPOSITORY}"
      GIT_TAG "${BKMAIL_BEXEC_GIT_TAG}"
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(bexec)
    message(
      STATUS
        "Fetched bexec ${BKMAIL_BEXEC_GIT_TAG} from ${BKMAIL_BEXEC_GIT_REPOSITORY}"
    )
  endif()

  if(NOT TARGET bexec::bexec)
    if(TARGET bexec)
      add_library(bexec::bexec ALIAS bexec)
    else()
      message(FATAL_ERROR
              "bexec was resolved, but target bexec::bexec was not found")
    endif()
  endif()
endfunction()
