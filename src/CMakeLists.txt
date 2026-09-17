add_library(bkmail)
add_library(bkmail::bkmail ALIAS bkmail)

target_compile_features(bkmail PUBLIC cxx_std_20)
target_link_libraries(
  bkmail
  PUBLIC $<BUILD_INTERFACE:bexec::bexec>
         $<INSTALL_INTERFACE:bexec::bexec>
         $<BUILD_INTERFACE:bnio::bnio>
         $<INSTALL_INTERFACE:bnio::bnio>)

target_compile_definitions(bkmail PRIVATE
                           BKMAIL_VERSION_STR="${PROJECT_VERSION}")

target_include_directories(
  bkmail
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set(BKMAIL_SOURCES version.cpp error.cpp capabilities.cpp)
target_sources(bkmail PRIVATE ${BKMAIL_SOURCES} ${BKMAIL_PUBLIC_HEADERS})

get_target_property(BKMAIL_TARGET_TYPE bkmail TYPE)
if(BKMAIL_TARGET_TYPE STREQUAL "SHARED_LIBRARY")
  target_compile_definitions(bkmail PUBLIC BKMAIL_SHARED_LIBRARY)
  target_compile_definitions(bkmail PRIVATE BKMAIL_BUILDING_LIBRARY)
  set_target_properties(
    bkmail
    PROPERTIES VERSION "${PROJECT_VERSION}"
               SOVERSION "${PROJECT_VERSION_MAJOR}"
               CXX_VISIBILITY_PRESET hidden
               VISIBILITY_INLINES_HIDDEN YES)
endif()

bkmail_set_project_warnings(bkmail)

if(BKMAIL_INSTALL)
  install(
    TARGETS bkmail
    EXPORT bkmailTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT development
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT runtime
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime)
  install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/bkmail"
          DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
          COMPONENT development)
  install(
    EXPORT bkmailTargets
    FILE bkmailTargets.cmake
    NAMESPACE bkmail::
    DESTINATION "${BKMAIL_INSTALL_CMAKEDIR}"
    COMPONENT development)
endif()
