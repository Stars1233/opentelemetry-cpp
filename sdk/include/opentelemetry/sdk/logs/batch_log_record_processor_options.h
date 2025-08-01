// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstddef>

#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{

namespace logs
{

/**
 * Struct to hold batch LogRecordProcessor options.
 */
struct BatchLogRecordProcessorOptions
{
  /**
   * The maximum buffer/queue size. After the size is reached, spans are
   * dropped.
   */
  size_t max_queue_size = 2048;

  /* The time interval between two consecutive exports. */
  std::chrono::milliseconds schedule_delay_millis = std::chrono::milliseconds(5000);

  /**
   * The maximum batch size of every export. It must be smaller or
   * equal to max_queue_size.
   */
  size_t max_export_batch_size = 512;
};

}  // namespace logs
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
