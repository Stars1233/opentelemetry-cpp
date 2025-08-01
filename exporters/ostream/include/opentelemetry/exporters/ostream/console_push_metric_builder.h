// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>

#include "opentelemetry/sdk/configuration/console_push_metric_exporter_builder.h"
#include "opentelemetry/sdk/configuration/console_push_metric_exporter_configuration.h"
#include "opentelemetry/sdk/configuration/registry.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace metrics
{

class OPENTELEMETRY_EXPORT ConsolePushMetricBuilder
    : public opentelemetry::sdk::configuration::ConsolePushMetricExporterBuilder
{
public:
  static void Register(opentelemetry::sdk::configuration::Registry *registry);

  std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> Build(
      const opentelemetry::sdk::configuration::ConsolePushMetricExporterConfiguration *model)
      const override;
};

}  // namespace metrics
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
