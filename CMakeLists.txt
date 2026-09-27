# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.28...3.31)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

if(NOT OS_WINDOWS)
  message(FATAL_ERROR "Easy Multistream currently supports Windows x64 only.")
endif()

include(compilerconfig)
include(defaults)
include(helpers)
include(CTest)

add_library(${CMAKE_PROJECT_NAME} MODULE)
add_library(${CMAKE_PROJECT_NAME}-core STATIC)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE ${CMAKE_PROJECT_NAME}-core OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets
)

target_link_libraries(${CMAKE_PROJECT_NAME}-core PUBLIC OBS::libobs PRIVATE Advapi32)

target_include_directories(${CMAKE_PROJECT_NAME}-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

configure_file(src/version.hpp.in generated/version.hpp @ONLY)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/dock-view.cpp
    src/dock-view.hpp
    src/plugin-main.cpp
    src/settings-controller.cpp
    src/settings-controller.hpp
    src/version.hpp.in
)

target_sources(
  ${CMAKE_PROJECT_NAME}-core
  PRIVATE
    src/credential-vault.hpp
    src/secure-buffer.cpp
    src/secure-buffer.hpp
    src/settings.cpp
    src/settings.hpp
    src/windows-credential-vault.cpp
    src/windows-credential-vault.hpp
)

set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

if(BUILD_TESTING)
  add_executable(${CMAKE_PROJECT_NAME}-tests)
  target_sources(${CMAKE_PROJECT_NAME}-tests PRIVATE tests/unit-tests.cpp)
  target_link_libraries(${CMAKE_PROJECT_NAME}-tests PRIVATE ${CMAKE_PROJECT_NAME}-core)

  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}-tests
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different $<TARGET_RUNTIME_DLLS:${CMAKE_PROJECT_NAME}-tests>
      $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}-tests>
    COMMAND_EXPAND_LISTS
    COMMENT "Copy test runtime dependencies"
    VERBATIM
  )

  add_test(NAME ${CMAKE_PROJECT_NAME}-unit COMMAND ${CMAKE_PROJECT_NAME}-tests)

  add_executable(${CMAKE_PROJECT_NAME}-ui-tests)
  target_sources(
    ${CMAKE_PROJECT_NAME}-ui-tests
    PRIVATE src/dock-view.cpp src/dock-view.hpp tests/ui-tests.cpp
  )
  target_include_directories(${CMAKE_PROJECT_NAME}-ui-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(${CMAKE_PROJECT_NAME}-ui-tests PRIVATE Qt6::Core Qt6::Widgets)

  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}-ui-tests
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different $<TARGET_RUNTIME_DLLS:${CMAKE_PROJECT_NAME}-ui-tests>
      $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}-ui-tests>
    COMMAND_EXPAND_LISTS
    COMMENT "Copy UI test runtime dependencies"
    VERBATIM
  )

  if(TARGET Qt6::QMinimalIntegrationPlugin)
    add_custom_command(
      TARGET ${CMAKE_PROJECT_NAME}-ui-tests
      POST_BUILD
      COMMAND
        "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}-ui-tests>/platforms"
      COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different $<TARGET_FILE:Qt6::QMinimalIntegrationPlugin>
        "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}-ui-tests>/platforms"
      COMMENT "Copy the minimal Qt platform plugin"
      VERBATIM
    )
  endif()

  add_test(
    NAME ${CMAKE_PROJECT_NAME}-ui
    COMMAND ${CMAKE_PROJECT_NAME}-ui-tests -platform minimal
  )
  add_test(
    NAME ${CMAKE_PROJECT_NAME}-locales
    COMMAND
      "${CMAKE_COMMAND}" -DPROJECT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR} -P
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/check-locales.cmake"
  )
endif()
