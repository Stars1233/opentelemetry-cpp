# Copyright The OpenTelemetry Authors
# SPDX-License-Identifier: Apache-2.0

if(WITH_HTTP_CLIENT_CURL)
  set(FILENAME curl_http_test)
  add_compile_definitions(WITH_CURL)
  add_executable(${FILENAME} ${FILENAME}.cc)
  target_link_libraries(${FILENAME} ${GMOCK_LIB} ${GTEST_BOTH_LIBRARIES}
                        ${CMAKE_THREAD_LIBS_INIT})

  unset(CURL_IMPORTED_TARGET_NAME)

  foreach(FIND_CURL_IMPORTED_TARGET CURL::libcurl CURL::libcurl_shared
                                    CURL::libcurl_static)
    if(TARGET ${FIND_CURL_IMPORTED_TARGET})
      set(CURL_IMPORTED_TARGET_NAME ${FIND_CURL_IMPORTED_TARGET})
      break()
    endif()
  endforeach()

  if(TARGET ${CURL_IMPORTED_TARGET_NAME})
    target_link_libraries(${FILENAME} opentelemetry_http_client_curl
                          opentelemetry_common ${CURL_IMPORTED_TARGET_NAME})
  else()
    include_directories(${CURL_INCLUDE_DIRS})
    target_link_libraries(${FILENAME} ${CURL_LIBRARIES}
                          opentelemetry_http_client_curl opentelemetry_common)
  endif()

  gtest_add_tests(
    TARGET ${FILENAME}
    TEST_PREFIX ext.http.curl.
    TEST_LIST ${FILENAME})
endif()

set(URL_PARSER_FILENAME url_parser_test)
add_executable(${URL_PARSER_FILENAME} ${URL_PARSER_FILENAME}.cc)
target_link_libraries(${URL_PARSER_FILENAME} opentelemetry_api ${GMOCK_LIB}
                      ${GTEST_BOTH_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
gtest_add_tests(
  TARGET ${URL_PARSER_FILENAME}
  TEST_PREFIX ext.http.urlparser.
  TEST_LIST ${URL_PARSER_FILENAME})
