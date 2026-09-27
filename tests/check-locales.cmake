# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED PROJECT_SOURCE_DIR)
  message(FATAL_ERROR "PROJECT_SOURCE_DIR is required")
endif()

function(read_locale_keys path output_variable)
  file(STRINGS "${path}" locale_lines ENCODING UTF-8)
  set(keys)
  foreach(line IN LISTS locale_lines)
    if(line MATCHES "^([A-Za-z0-9_.-]+)=")
      list(APPEND keys "${CMAKE_MATCH_1}")
    endif()
  endforeach()

  set(unique_keys "${keys}")
  list(REMOVE_DUPLICATES unique_keys)
  list(LENGTH keys key_count)
  list(LENGTH unique_keys unique_key_count)
  if(NOT key_count EQUAL unique_key_count)
    message(FATAL_ERROR "Duplicate locale key in ${path}")
  endif()

  list(SORT unique_keys)
  set(${output_variable} "${unique_keys}" PARENT_SCOPE)
endfunction()

read_locale_keys("${PROJECT_SOURCE_DIR}/data/locale/en-US.ini" english_keys)
read_locale_keys("${PROJECT_SOURCE_DIR}/data/locale/ja-JP.ini" japanese_keys)

if(NOT english_keys STREQUAL japanese_keys)
  message(FATAL_ERROR "en-US.ini and ja-JP.ini must contain exactly the same keys")
endif()

file(READ "${PROJECT_SOURCE_DIR}/src/plugin-main.cpp" plugin_source)
string(REGEX MATCHALL "moduleText\\(\"[A-Za-z0-9_.-]+\"\\)" module_text_calls "${plugin_source}")
foreach(call IN LISTS module_text_calls)
  string(REGEX REPLACE "^moduleText\\(\"([^\"]+)\"\\)$" "\\1" key "${call}")
  if(NOT key IN_LIST english_keys)
    message(FATAL_ERROR "plugin-main.cpp references missing locale key: ${key}")
  endif()
endforeach()
