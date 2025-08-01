// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/exporters/otlp/otlp_grpc_client_options.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

/**
 * Struct to hold OTLP GRPC traces exporter options.
 *
 * See
 * https://github.com/open-telemetry/opentelemetry-proto/blob/main/docs/specification.md#otlpgrpc
 *
 * See
 * https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/protocol/exporter.md
 */
struct OPENTELEMETRY_EXPORT OtlpGrpcExporterOptions : public OtlpGrpcClientOptions
{
  /** Lookup environment variables. */
  OtlpGrpcExporterOptions();
  /** No defaults. */
  OtlpGrpcExporterOptions(void *);
  OtlpGrpcExporterOptions(const OtlpGrpcExporterOptions &)            = default;
  OtlpGrpcExporterOptions(OtlpGrpcExporterOptions &&)                 = default;
  OtlpGrpcExporterOptions &operator=(const OtlpGrpcExporterOptions &) = default;
  OtlpGrpcExporterOptions &operator=(OtlpGrpcExporterOptions &&)      = default;
  ~OtlpGrpcExporterOptions() override;
};

}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
