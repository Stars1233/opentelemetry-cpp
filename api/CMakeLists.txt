# Copyright The OpenTelemetry Authors
# SPDX-License-Identifier: Apache-2.0

add_library(opentelemetry_api INTERFACE)
target_include_directories(
  opentelemetry_api
  INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>"
            "$<INSTALL_INTERFACE:include>")

set_target_properties(opentelemetry_api PROPERTIES EXPORT_NAME api)

unset(TARGET_DEPS)

if(BUILD_TESTING)
  add_subdirectory(test)
endif()

if(WITH_NO_DEPRECATED_CODE)
  target_compile_definitions(opentelemetry_api
                             INTERFACE OPENTELEMETRY_NO_DEPRECATED_CODE)
endif()

if(WITH_STL STREQUAL "OFF")
  message(STATUS "Building WITH_STL=OFF")
elseif(WITH_STL STREQUAL "CXX11")
  message(STATUS "Building WITH_STL=CXX11")
  target_compile_definitions(opentelemetry_api
                             INTERFACE OPENTELEMETRY_STL_VERSION=2011)
elseif(WITH_STL STREQUAL "CXX14")
  message(STATUS "Building WITH_STL=CXX14")
  target_compile_definitions(opentelemetry_api
                             INTERFACE OPENTELEMETRY_STL_VERSION=2014)
elseif(WITH_STL STREQUAL "CXX17")
  message(STATUS "Building WITH_STL=CXX17")
  target_compile_definitions(opentelemetry_api
                             INTERFACE OPENTELEMETRY_STL_VERSION=2017)
elseif(WITH_STL STREQUAL "CXX20")
  message(STATUS "Building WITH_STL=CXX20")
  target_compile_definitions(opentelemetry_api
                             INTERFACE OPENTELEMETRY_STL_VERSION=2020)
elseif(WITH_STL STREQUAL "CXX23")
  message(STATUS "Building WITH_STL=CXX23")
  target_compile_definitions(opentelemetry_api
                             INTERFACE OPENTELEMETRY_STL_VERSION=2023)
elseif(WITH_STL STREQUAL "CXX26")
  message(STATUS "Building WITH_STL=CXX26")
  target_compile_definitions(opentelemetry_api
                             INTERFACE OPENTELEMETRY_STL_VERSION=2026)
elseif(WITH_STL STREQUAL "ON")
  message(STATUS "Building WITH_STL=ON")
  # "ON" corresponds to "CXX23" at this time.
  target_compile_definitions(opentelemetry_api
                             INTERFACE OPENTELEMETRY_STL_VERSION=2023)
else()
  message(
    FATAL_ERROR
      "WITH_STL must be ON, OFF, CXX11, CXX14, CXX17, CXX20, CXX23 or CXX26")
endif()

if(WITH_GSL)
  target_compile_definitions(opentelemetry_api INTERFACE HAVE_GSL)
  target_link_libraries(opentelemetry_api INTERFACE Microsoft.GSL::GSL)
  list(APPEND TARGET_DEPS "gsl")
endif()

if(WITH_NO_GETENV)
  target_compile_definitions(opentelemetry_api INTERFACE NO_GETENV)
endif()

if(WIN32)
  if(WITH_ETW)
    target_compile_definitions(opentelemetry_api INTERFACE HAVE_MSGPACK)
  endif()
endif()

if(WITH_ASYNC_EXPORT_PREVIEW)
  target_compile_definitions(opentelemetry_api INTERFACE ENABLE_ASYNC_EXPORT)
endif()

target_compile_definitions(
  opentelemetry_api
  INTERFACE OPENTELEMETRY_ABI_VERSION_NO=${OPENTELEMETRY_ABI_VERSION_NO})

if(WITH_OTLP_RETRY_PREVIEW)
  target_compile_definitions(opentelemetry_api
                             INTERFACE ENABLE_OTLP_RETRY_PREVIEW)
endif()

if(WITH_OTLP_GRPC_SSL_MTLS_PREVIEW)
  target_compile_definitions(opentelemetry_api
                             INTERFACE ENABLE_OTLP_GRPC_SSL_MTLS_PREVIEW)
endif()

if(WITH_OTLP_GRPC_CREDENTIAL_PREVIEW)
  target_compile_definitions(opentelemetry_api
                             INTERFACE ENABLE_OTLP_GRPC_CREDENTIAL_PREVIEW)
endif()

if(WITH_METRICS_EXEMPLAR_PREVIEW)
  target_compile_definitions(opentelemetry_api
                             INTERFACE ENABLE_METRICS_EXEMPLAR_PREVIEW)
endif()

if(WITH_THREAD_INSTRUMENTATION_PREVIEW)
  target_compile_definitions(opentelemetry_api
                             INTERFACE ENABLE_THREAD_INSTRUMENTATION_PREVIEW)
endif()

if(WITH_OTLP_HTTP_COMPRESSION)
  target_compile_definitions(opentelemetry_api
                             INTERFACE ENABLE_OTLP_COMPRESSION_PREVIEW)
endif()

if(APPLE)
  target_link_libraries(opentelemetry_api INTERFACE "-framework CoreFoundation")
endif()

otel_add_component(
  COMPONENT
  api
  TARGETS
  opentelemetry_api
  FILES_DIRECTORY
  "include/opentelemetry"
  FILES_DESTINATION
  "include"
  FILES_MATCHING
  PATTERN
  "*.h")

include(${PROJECT_SOURCE_DIR}/cmake/pkgconfig.cmake)

if(OPENTELEMETRY_INSTALL)
  opentelemetry_add_pkgconfig(
    api "OpenTelemetry API"
    "A header-only library to support instrumentation with OpenTelemetry."
    "${TARGET_DEPS}")
endif()
