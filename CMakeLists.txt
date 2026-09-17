# @file CMakeLists.txt
# @brief Top-level CMake build definition for bkmail.
# @author Haoming Bai <haomingbai@hotmail.com>
# @date   2026-09-15
#
# Copyright © 2026 Haoming Bai
# SPDX-License-Identifier: MIT
#
# @details
# bkmail is a library built on top of bnio and bexec. Both dependencies are
# resolved through dedicated CMake modules supporting AUTO, FIND_PACKAGE,
# SOURCE, and FETCH providers. bexec is resolved before bnio on purpose:
# bnio also depends on bexec, and every resolver short-circuits when the
# target already exists, so bexec is never introduced twice.

cmake_minimum_required(VERSION 3.20)

set(BKMAIL_VERSION "0.1.0" CACHE STRING "bkmail package version")

project(
  bkmail
  VERSION "${BKMAIL_VERSION}"
  DESCRIPTION "A small e-mail library built on bnio and bexec"
  LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/bexec.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/bnio.cmake")

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(BKMAIL_IS_TOP_LEVEL ON)
else()
  set(BKMAIL_IS_TOP_LEVEL OFF)
endif()

option(BUILD_SHARED_LIBS "Build shared libraries" OFF)
option(BKMAIL_BUILD_TESTS "Build bkmail tests" ${BKMAIL_IS_TOP_LEVEL})
option(BKMAIL_BUILD_EXAMPLES "Build bkmail examples" ${BKMAIL_IS_TOP_LEVEL})
option(BKMAIL_ENABLE_WARNINGS "Enable compiler warnings for bkmail targets" ON)
option(BKMAIL_ENABLE_TSAN "Build bkmail targets with ThreadSanitizer" OFF)
option(BKMAIL_ENABLE_COVERAGE
       "Instrument bkmail targets for coverage reporting" OFF)
option(BKMAIL_INSTALL "Generate bkmail installation rules"
       ${BKMAIL_IS_TOP_LEVEL})
option(BKMAIL_PACKAGE "Generate CPack packaging rules" OFF)

if(BKMAIL_INSTALL)
  set(
    BKMAIL_INSTALL_CMAKEDIR
    "${CMAKE_INSTALL_LIBDIR}/cmake/bkmail"
    CACHE STRING "Directory in which to install the bkmail CMake package")
endif()

if(BKMAIL_BUILD_TESTS)
  include(CTest)
  enable_testing()
endif()

# ---- Dependency resolution ----
#
# Resolve bexec first so that bnio (which also depends on bexec) reuses the
# very same target instead of introducing a second copy. When bnio is
# resolved via find_package, its package config skips its own bexec lookup
# because the target already exists here as well.
bkmail_resolve_bexec_dependency()
bkmail_resolve_bnio_dependency()

# ---- Installability guard ----
#
# install(EXPORT) of bkmail may only reference imported targets, i.e. bexec
# and bnio coming from installed packages found via find_package. When either
# dependency is a plain build-tree target (SOURCE or FETCH provider), a
# consumer of the installed package could not resolve it, so installation
# rules are disabled with an explanation. Consume bkmail as a subdirectory
# (or via FetchContent) in that case; install it in a standalone
# configuration where find_package provides both dependencies.
if(BKMAIL_INSTALL)
  set(BKMAIL_DEPS_IMPORTABLE ON)
  foreach(_bkmail_dep IN ITEMS bexec bnio)
    if(TARGET "${_bkmail_dep}::${_bkmail_dep}")
      get_target_property(_bkmail_dep_imported
                          "${_bkmail_dep}::${_bkmail_dep}" IMPORTED)
      if(NOT _bkmail_dep_imported)
        set(BKMAIL_DEPS_IMPORTABLE OFF)
      endif()
    else()
      set(BKMAIL_DEPS_IMPORTABLE OFF)
    endif()
  endforeach()
  if(NOT BKMAIL_DEPS_IMPORTABLE)
    message(STATUS
            "BKMAIL_INSTALL disabled: bexec/bnio are build-tree targets "
            "(SOURCE/FETCH providers), not installed packages. Install "
            "bkmail in a standalone configuration where find_package "
            "provides both dependencies.")
    set(BKMAIL_INSTALL OFF)
  endif()
  unset(_bkmail_dep)
  unset(_bkmail_dep_imported)
endif()

function(bkmail_set_project_warnings target)
  if(NOT BKMAIL_ENABLE_WARNINGS)
    return()
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic
                                             -Wconversion)
  endif()
endfunction()

if(BKMAIL_ENABLE_TSAN)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
  else()
    message(FATAL_ERROR "BKMAIL_ENABLE_TSAN requires GCC or Clang")
  endif()
endif()

if(BKMAIL_ENABLE_COVERAGE)
  if(BKMAIL_ENABLE_TSAN)
    message(FATAL_ERROR
            "BKMAIL_ENABLE_COVERAGE cannot be combined with BKMAIL_ENABLE_TSAN")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-O0 -g --coverage)
    add_link_options(--coverage)
  else()
    message(FATAL_ERROR
            "BKMAIL_ENABLE_COVERAGE requires GCC, Clang, or AppleClang")
  endif()
endif()

add_subdirectory(include)
add_subdirectory(src)

if(BKMAIL_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(BKMAIL_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(BKMAIL_INSTALL)
  configure_package_config_file(
    cmake/bkmailConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/bkmailConfig.cmake"
    INSTALL_DESTINATION "${BKMAIL_INSTALL_CMAKEDIR}")
  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/bkmailConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)
  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/bkmailConfig.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/bkmailConfigVersion.cmake"
    DESTINATION "${BKMAIL_INSTALL_CMAKEDIR}"
    COMPONENT development)

  if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    file(
      RELATIVE_PATH BKMAIL_PC_PREFIX_RELATIVE
      "${CMAKE_INSTALL_LIBDIR}/pkgconfig" "${CMAKE_INSTALL_PREFIX}")
  else()
    file(
      RELATIVE_PATH BKMAIL_PC_PREFIX_RELATIVE
      "/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/")
  endif()

  set(BKMAIL_PC_REQUIRES
      "bexec >= ${BKMAIL_BEXEC_MIN_VERSION}, bnio >= ${BKMAIL_BNIO_MIN_VERSION}")
  if(BUILD_SHARED_LIBS)
    set(BKMAIL_PC_SHARED_CFLAGS " -DBKMAIL_SHARED_LIBRARY")
  else()
    set(BKMAIL_PC_SHARED_CFLAGS "")
  endif()
  configure_file(cmake/bkmail.pc.in "${CMAKE_CURRENT_BINARY_DIR}/bkmail.pc"
                 @ONLY)
  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/bkmail.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
    COMPONENT development)
  install(FILES LICENSE README.md
          DESTINATION "${CMAKE_INSTALL_DOCDIR}"
          COMPONENT development)
endif()

# ---- CPack packaging ----
if(BKMAIL_PACKAGE)
  set(CPACK_PACKAGE_NAME "bkmail")
  set(CPACK_PACKAGE_VENDOR "Haoming Bai")
  set(CPACK_PACKAGE_CONTACT "Haoming Bai <haomingbai@hotmail.com>")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
  set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/haomingbai/bkmail")
  set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}/packages")
  set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
  set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
  set(CPACK_DEBIAN_PACKAGE_SECTION "libdevel")
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)

  set(CPACK_RPM_PACKAGE_LICENSE "MIT")
  set(CPACK_RPM_PACKAGE_GROUP "Development/Libraries")
  set(CPACK_RPM_PACKAGE_RELEASE 1)
  set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
  # Do not claim ownership of directories owned by other packages
  # (pkgconf owns the pkgconfig directory, cmake owns the cmake directory).
  if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(BKMAIL_PACKAGE_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
  else()
    set(
      BKMAIL_PACKAGE_LIBDIR
      "${CPACK_PACKAGING_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}"
    )
  endif()
  set(
    CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    "${BKMAIL_PACKAGE_LIBDIR}/pkgconfig"
    "${BKMAIL_PACKAGE_LIBDIR}/cmake"
  )

  set(CPACK_GENERATOR "DEB;RPM")
  include(CPack)
endif()
