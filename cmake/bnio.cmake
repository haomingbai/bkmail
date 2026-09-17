include_guard(GLOBAL)

set(BKMAIL_BNIO_PROVIDER
    "AUTO"
    CACHE STRING
          "How to resolve bnio: AUTO, FIND_PACKAGE, SOURCE, or FETCH")
set_property(CACHE BKMAIL_BNIO_PROVIDER PROPERTY STRINGS AUTO FIND_PACKAGE
                                                 SOURCE FETCH)
set(BKMAIL_BNIO_MIN_VERSION
    "0.2.0"
    CACHE STRING "Minimum accepted bnio package version")
set(BKMAIL_BNIO_SOURCE_DIR
    ""
    CACHE PATH "Path to a local bnio source checkout")
set(BKMAIL_BNIO_GIT_REPOSITORY
    "https://github.com/haomingbai/bnio.git"
    CACHE STRING "Git repository used by the FETCH provider")
set(BKMAIL_BNIO_GIT_TAG
    "v0.2.0"
    CACHE STRING "Git ref used by the FETCH provider")

function(bkmail_resolve_bnio_dependency)
  if(TARGET bnio::bnio)
    message(STATUS "Using the existing bnio::bnio target")
    return()
  endif()

  if(TARGET bnio)
    add_library(bnio::bnio ALIAS bnio)
    message(STATUS "Using the existing bnio target")
    return()
  endif()

  string(TOUPPER "${BKMAIL_BNIO_PROVIDER}" _bkmail_bnio_provider)
  set(_bkmail_bnio_providers AUTO FIND_PACKAGE SOURCE FETCH)
  if(NOT _bkmail_bnio_provider IN_LIST _bkmail_bnio_providers)
    message(
      FATAL_ERROR
        "BKMAIL_BNIO_PROVIDER must be AUTO, FIND_PACKAGE, SOURCE, or FETCH")
  endif()

  if(_bkmail_bnio_provider STREQUAL "AUTO")
    if(BKMAIL_BNIO_SOURCE_DIR)
      set(_bkmail_bnio_provider SOURCE)
    else()
      find_package(bnio ${BKMAIL_BNIO_MIN_VERSION} CONFIG QUIET)
      if(TARGET bnio::bnio OR TARGET bnio)
        set(_bkmail_bnio_provider FIND_PACKAGE)
      else()
        set(_bkmail_bnio_provider FETCH)
      endif()
    endif()
  endif()

  if(_bkmail_bnio_provider STREQUAL "FIND_PACKAGE")
    if(NOT TARGET bnio::bnio AND NOT TARGET bnio)
      find_package(bnio ${BKMAIL_BNIO_MIN_VERSION} CONFIG REQUIRED)
    endif()
    message(STATUS "Resolved bnio with find_package")
  elseif(_bkmail_bnio_provider STREQUAL "SOURCE")
    if(NOT BKMAIL_BNIO_SOURCE_DIR)
      message(
        FATAL_ERROR
          "BKMAIL_BNIO_SOURCE_DIR is required when BKMAIL_BNIO_PROVIDER=SOURCE")
    endif()
    get_filename_component(_bkmail_bnio_source_dir "${BKMAIL_BNIO_SOURCE_DIR}"
                           ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    if(NOT EXISTS "${_bkmail_bnio_source_dir}/CMakeLists.txt")
      message(
        FATAL_ERROR
          "BKMAIL_BNIO_SOURCE_DIR must point to a bnio source tree")
    endif()
    bkmail_bnio_add_subdirectory("${_bkmail_bnio_source_dir}")
    message(STATUS "Resolved bnio from ${_bkmail_bnio_source_dir}")
  elseif(_bkmail_bnio_provider STREQUAL "FETCH")
    set(BNIO_BUILD_TESTS OFF CACHE BOOL "Build bnio tests" FORCE)
    set(BNIO_BUILD_EXAMPLES OFF CACHE BOOL "Build bnio examples" FORCE)
    set(BNIO_BUILD_BENCHMARKS OFF CACHE BOOL "Build bnio benchmark executables"
                                     FORCE)
    set(BNIO_BUILD_ASIO_EXAMPLES
        OFF CACHE BOOL "Build standalone Asio echo server example" FORCE)
    set(BNIO_INSTALL OFF CACHE BOOL "Generate bnio installation rules" FORCE)
    include(FetchContent)
    FetchContent_Declare(
      bnio
      GIT_REPOSITORY "${BKMAIL_BNIO_GIT_REPOSITORY}"
      GIT_TAG "${BKMAIL_BNIO_GIT_TAG}"
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(bnio)
    message(
      STATUS
        "Fetched bnio ${BKMAIL_BNIO_GIT_TAG} from ${BKMAIL_BNIO_GIT_REPOSITORY}")
  endif()

  if(NOT TARGET bnio::bnio)
    if(TARGET bnio)
      add_library(bnio::bnio ALIAS bnio)
    else()
      message(FATAL_ERROR
              "bnio was resolved, but target bnio::bnio was not found")
    endif()
  endif()
endfunction()

# Adds an existing bnio source tree as a subdirectory with bnio's own tests,
# examples, benchmarks, and installation rules disabled, mirroring what bnio's
# package config exposes.
function(bkmail_bnio_add_subdirectory bnio_source_dir)
  set(BNIO_BUILD_TESTS OFF CACHE BOOL "Build bnio tests" FORCE)
  set(BNIO_BUILD_EXAMPLES OFF CACHE BOOL "Build bnio examples" FORCE)
  set(BNIO_BUILD_BENCHMARKS OFF CACHE BOOL "Build bnio benchmark executables"
                                     FORCE)
  set(BNIO_BUILD_ASIO_EXAMPLES OFF CACHE BOOL "Build standalone Asio echo server example"
                                     FORCE)
  set(BNIO_INSTALL OFF CACHE BOOL "Generate bnio installation rules" FORCE)
  add_subdirectory("${bnio_source_dir}" "${PROJECT_BINARY_DIR}/_deps/bnio-build"
                   EXCLUDE_FROM_ALL)
endfunction()
