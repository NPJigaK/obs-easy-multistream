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
add_library(${CMAKE_PROJECT_NAME}-secure-core STATIC)
add_library(${CMAKE_PROJECT_NAME}-youtube-destination STATIC)
add_library(${CMAKE_PROJECT_NAME}-core STATIC)
add_library(${CMAKE_PROJECT_NAME}-session-core STATIC)
add_library(${CMAKE_PROJECT_NAME}-oauth-core STATIC)
add_library(${CMAKE_PROJECT_NAME}-oauth-loopback STATIC)
add_library(${CMAKE_PROJECT_NAME}-oauth-token-transport STATIC)
add_library(${CMAKE_PROJECT_NAME}-youtube-discovery STATIC)
add_library(${CMAKE_PROJECT_NAME}-youtube-stream-resolver STATIC)

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets)

target_link_libraries(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    ${CMAKE_PROJECT_NAME}-core
    ${CMAKE_PROJECT_NAME}-session-core
    ${CMAKE_PROJECT_NAME}-oauth-core
    OBS::libobs
    OBS::obs-frontend-api
    Qt6::Core
    Qt6::Widgets
)

target_sources(
  ${CMAKE_PROJECT_NAME}-secure-core
  PRIVATE src/secure-buffer.cpp src/secure-buffer.hpp
)
target_include_directories(${CMAKE_PROJECT_NAME}-secure-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_sources(
  ${CMAKE_PROJECT_NAME}-youtube-destination
  PRIVATE src/youtube-destination.cpp src/youtube-destination.hpp
)
target_include_directories(${CMAKE_PROJECT_NAME}-youtube-destination PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(
  ${CMAKE_PROJECT_NAME}-core
  PUBLIC OBS::libobs ${CMAKE_PROJECT_NAME}-secure-core ${CMAKE_PROJECT_NAME}-youtube-destination
  PRIVATE Advapi32
)

target_include_directories(${CMAKE_PROJECT_NAME}-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_sources(
  ${CMAKE_PROJECT_NAME}-session-core
  PRIVATE
    src/session-coordinator.cpp
    src/session-coordinator.hpp
    src/youtube-account-coordinator.cpp
    src/youtube-account-coordinator.hpp
)
target_include_directories(${CMAKE_PROJECT_NAME}-session-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_sources(
  ${CMAKE_PROJECT_NAME}-oauth-core
  PRIVATE src/google-oauth-protocol.cpp src/google-oauth-protocol.hpp
)
target_include_directories(${CMAKE_PROJECT_NAME}-oauth-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
  ${CMAKE_PROJECT_NAME}-oauth-core
  PUBLIC Qt6::Core
  PRIVATE ${CMAKE_PROJECT_NAME}-secure-core
)

target_sources(
  ${CMAKE_PROJECT_NAME}-oauth-loopback
  PRIVATE src/google-oauth-loopback-listener.cpp src/google-oauth-loopback-listener.hpp
)
target_include_directories(${CMAKE_PROJECT_NAME}-oauth-loopback PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
  ${CMAKE_PROJECT_NAME}-oauth-loopback
  PUBLIC ${CMAKE_PROJECT_NAME}-oauth-core Qt6::Core Qt6::Network
  PRIVATE Ws2_32
)

target_sources(
  ${CMAKE_PROJECT_NAME}-oauth-token-transport
  PRIVATE src/google-oauth-token-transport.cpp src/google-oauth-token-transport.hpp
)
target_include_directories(
  ${CMAKE_PROJECT_NAME}-oauth-token-transport
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(
  ${CMAKE_PROJECT_NAME}-oauth-token-transport
  PUBLIC ${CMAKE_PROJECT_NAME}-oauth-core Qt6::Core Qt6::Network
)

target_sources(
  ${CMAKE_PROJECT_NAME}-youtube-discovery
  PRIVATE src/youtube-api-discovery.cpp src/youtube-api-discovery.hpp
)
target_include_directories(
  ${CMAKE_PROJECT_NAME}-youtube-discovery
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(
  ${CMAKE_PROJECT_NAME}-youtube-discovery
  PUBLIC ${CMAKE_PROJECT_NAME}-secure-core Qt6::Core Qt6::Network
)

target_sources(
  ${CMAKE_PROJECT_NAME}-youtube-stream-resolver
  PRIVATE src/youtube-api-stream-resolver.cpp src/youtube-api-stream-resolver.hpp
)
target_include_directories(
  ${CMAKE_PROJECT_NAME}-youtube-stream-resolver
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(
  ${CMAKE_PROJECT_NAME}-youtube-stream-resolver
  PUBLIC ${CMAKE_PROJECT_NAME}-secure-core ${CMAKE_PROJECT_NAME}-youtube-destination Qt6::Core Qt6::Network
)

configure_file(src/version.hpp.in generated/version.hpp @ONLY)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/dock-visibility.cpp
    src/dock-visibility.hpp
    src/dock-view.cpp
    src/dock-view.hpp
    src/plugin-main.cpp
    src/runtime-controller.cpp
    src/runtime-controller.hpp
    src/settings-controller.cpp
    src/settings-controller.hpp
    src/version.hpp.in
    src/youtube-output-adapter.cpp
    src/youtube-output-adapter.hpp
)

target_sources(
  ${CMAKE_PROJECT_NAME}-core
  PRIVATE
    src/credential-vault.hpp
    src/onboarding-state.cpp
    src/onboarding-state.hpp
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

  add_executable(${CMAKE_PROJECT_NAME}-session-tests)
  target_sources(${CMAKE_PROJECT_NAME}-session-tests PRIVATE tests/session-tests.cpp)
  target_link_libraries(${CMAKE_PROJECT_NAME}-session-tests PRIVATE ${CMAKE_PROJECT_NAME}-session-core)
  add_test(NAME ${CMAKE_PROJECT_NAME}-session COMMAND ${CMAKE_PROJECT_NAME}-session-tests)

  add_executable(${CMAKE_PROJECT_NAME}-youtube-account-tests)
  target_sources(${CMAKE_PROJECT_NAME}-youtube-account-tests PRIVATE tests/youtube-account-coordinator-tests.cpp)
  target_link_libraries(${CMAKE_PROJECT_NAME}-youtube-account-tests PRIVATE ${CMAKE_PROJECT_NAME}-session-core)
  add_test(NAME ${CMAKE_PROJECT_NAME}-youtube-account COMMAND ${CMAKE_PROJECT_NAME}-youtube-account-tests)

  add_executable(${CMAKE_PROJECT_NAME}-google-oauth-protocol-tests)
  target_sources(${CMAKE_PROJECT_NAME}-google-oauth-protocol-tests PRIVATE tests/google-oauth-protocol-tests.cpp)
  target_link_libraries(${CMAKE_PROJECT_NAME}-google-oauth-protocol-tests PRIVATE ${CMAKE_PROJECT_NAME}-oauth-core)

  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}-google-oauth-protocol-tests
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different
      $<TARGET_RUNTIME_DLLS:${CMAKE_PROJECT_NAME}-google-oauth-protocol-tests>
      $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}-google-oauth-protocol-tests>
    COMMAND_EXPAND_LISTS
    COMMENT "Copy Google OAuth protocol test runtime dependencies"
    VERBATIM
  )

  add_test(
    NAME ${CMAKE_PROJECT_NAME}-google-oauth-protocol
    COMMAND ${CMAKE_PROJECT_NAME}-google-oauth-protocol-tests
  )

  add_executable(${CMAKE_PROJECT_NAME}-google-oauth-loopback-tests)
  target_sources(
    ${CMAKE_PROJECT_NAME}-google-oauth-loopback-tests
    PRIVATE tests/google-oauth-loopback-listener-tests.cpp
  )
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}-google-oauth-loopback-tests
    PRIVATE ${CMAKE_PROJECT_NAME}-oauth-loopback Ws2_32
  )

  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}-google-oauth-loopback-tests
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different
      $<TARGET_RUNTIME_DLLS:${CMAKE_PROJECT_NAME}-google-oauth-loopback-tests>
      $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}-google-oauth-loopback-tests>
    COMMAND_EXPAND_LISTS
    COMMENT "Copy Google OAuth loopback test runtime dependencies"
    VERBATIM
  )

  add_test(
    NAME ${CMAKE_PROJECT_NAME}-google-oauth-loopback
    COMMAND ${CMAKE_PROJECT_NAME}-google-oauth-loopback-tests
  )

  add_executable(${CMAKE_PROJECT_NAME}-google-oauth-token-tests)
  target_sources(
    ${CMAKE_PROJECT_NAME}-google-oauth-token-tests
    PRIVATE tests/google-oauth-token-transport-tests.cpp
  )
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}-google-oauth-token-tests
    PRIVATE ${CMAKE_PROJECT_NAME}-oauth-token-transport
  )

  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}-google-oauth-token-tests
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different
      $<TARGET_RUNTIME_DLLS:${CMAKE_PROJECT_NAME}-google-oauth-token-tests>
      $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}-google-oauth-token-tests>
    COMMAND_EXPAND_LISTS
    COMMENT "Copy Google OAuth token transport test runtime dependencies"
    VERBATIM
  )

  add_test(
    NAME ${CMAKE_PROJECT_NAME}-google-oauth-token
    COMMAND ${CMAKE_PROJECT_NAME}-google-oauth-token-tests
  )

  add_executable(${CMAKE_PROJECT_NAME}-youtube-discovery-tests)
  target_sources(
    ${CMAKE_PROJECT_NAME}-youtube-discovery-tests
    PRIVATE tests/youtube-api-discovery-tests.cpp
  )
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}-youtube-discovery-tests
    PRIVATE ${CMAKE_PROJECT_NAME}-youtube-discovery
  )

  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}-youtube-discovery-tests
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different
      $<TARGET_RUNTIME_DLLS:${CMAKE_PROJECT_NAME}-youtube-discovery-tests>
      $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}-youtube-discovery-tests>
    COMMAND_EXPAND_LISTS
    COMMENT "Copy YouTube discovery test runtime dependencies"
    VERBATIM
  )

  add_test(
    NAME ${CMAKE_PROJECT_NAME}-youtube-discovery
    COMMAND ${CMAKE_PROJECT_NAME}-youtube-discovery-tests
  )

  add_executable(${CMAKE_PROJECT_NAME}-youtube-stream-resolver-tests)
  target_sources(
    ${CMAKE_PROJECT_NAME}-youtube-stream-resolver-tests
    PRIVATE tests/youtube-api-stream-resolver-tests.cpp
  )
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}-youtube-stream-resolver-tests
    PRIVATE ${CMAKE_PROJECT_NAME}-youtube-stream-resolver
  )

  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}-youtube-stream-resolver-tests
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different
      $<TARGET_RUNTIME_DLLS:${CMAKE_PROJECT_NAME}-youtube-stream-resolver-tests>
      $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}-youtube-stream-resolver-tests>
    COMMAND_EXPAND_LISTS
    COMMENT "Copy YouTube stream resolver test runtime dependencies"
    VERBATIM
  )

  add_test(
    NAME ${CMAKE_PROJECT_NAME}-youtube-stream-resolver
    COMMAND ${CMAKE_PROJECT_NAME}-youtube-stream-resolver-tests
  )

  add_executable(${CMAKE_PROJECT_NAME}-runtime-tests)
  target_sources(
    ${CMAKE_PROJECT_NAME}-runtime-tests
    PRIVATE src/runtime-controller.cpp src/runtime-controller.hpp tests/runtime-controller-tests.cpp
  )
  target_include_directories(${CMAKE_PROJECT_NAME}-runtime-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}-runtime-tests
    PRIVATE ${CMAKE_PROJECT_NAME}-core ${CMAKE_PROJECT_NAME}-session-core
  )

  add_custom_command(
    TARGET ${CMAKE_PROJECT_NAME}-runtime-tests
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different $<TARGET_RUNTIME_DLLS:${CMAKE_PROJECT_NAME}-runtime-tests>
      $<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}-runtime-tests>
    COMMAND_EXPAND_LISTS
    COMMENT "Copy runtime test dependencies"
    VERBATIM
  )

  add_test(NAME ${CMAKE_PROJECT_NAME}-runtime COMMAND ${CMAKE_PROJECT_NAME}-runtime-tests)

  add_executable(${CMAKE_PROJECT_NAME}-ui-tests)
  target_sources(
    ${CMAKE_PROJECT_NAME}-ui-tests
    PRIVATE
      src/dock-visibility.cpp
      src/dock-visibility.hpp
      src/dock-view.cpp
      src/dock-view.hpp
      tests/ui-tests.cpp
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

  set(test_runtime_environment)
  foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${prefix}/bin")
      list(APPEND test_runtime_environment "PATH=path_list_prepend:${prefix}/bin")
    endif()
  endforeach()
  if(NOT test_runtime_environment)
    message(FATAL_ERROR "No repository-local runtime dependency directories were found for tests")
  endif()
  set_tests_properties(
    ${CMAKE_PROJECT_NAME}-unit
    ${CMAKE_PROJECT_NAME}-ui
    ${CMAKE_PROJECT_NAME}-google-oauth-protocol
    ${CMAKE_PROJECT_NAME}-google-oauth-loopback
    ${CMAKE_PROJECT_NAME}-google-oauth-token
    ${CMAKE_PROJECT_NAME}-youtube-discovery
    ${CMAKE_PROJECT_NAME}-youtube-stream-resolver
    PROPERTIES ENVIRONMENT_MODIFICATION "${test_runtime_environment}"
  )
endif()
