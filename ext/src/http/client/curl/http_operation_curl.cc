// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <curl/curl.h>
#include <curl/curlver.h>

#ifdef ENABLE_OTLP_RETRY_PREVIEW
#  include <array>
#endif  // ENABLE_OTLP_RETRY_PREVIEW

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <random>
#include <ratio>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "opentelemetry/ext/http/client/curl/http_client_curl.h"
#include "opentelemetry/ext/http/client/curl/http_operation_curl.h"
#include "opentelemetry/ext/http/client/http_client.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/version.h"

// CURL_VERSION_BITS was added in CURL 7.43.0
#ifndef CURL_VERSION_BITS
#  define CURL_VERSION_BITS(x, y, z) ((x) << 16 | (y) << 8 | (z))
#endif

OPENTELEMETRY_BEGIN_NAMESPACE
namespace ext
{
namespace http
{
namespace client
{
namespace curl
{

class HttpOperationAccessor
{
public:
  OPENTELEMETRY_SANITIZER_NO_THREAD static std::thread::id GetThreadId(
      const HttpOperation::AsyncData &async_data)
  {
#if !(defined(OPENTELEMETRY_HAVE_THREAD_SANITIZER) && OPENTELEMETRY_HAVE_THREAD_SANITIZER)
    std::atomic_thread_fence(std::memory_order_acquire);
#endif
    return async_data.callback_thread;
  }

  OPENTELEMETRY_SANITIZER_NO_THREAD static void SetThreadId(HttpOperation::AsyncData &async_data,
                                                            std::thread::id thread_id)
  {
    async_data.callback_thread = thread_id;
#if !(defined(OPENTELEMETRY_HAVE_THREAD_SANITIZER) && OPENTELEMETRY_HAVE_THREAD_SANITIZER)
    std::atomic_thread_fence(std::memory_order_release);
#endif
  }
};

size_t HttpOperation::WriteMemoryCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
  HttpOperation *self = reinterpret_cast<HttpOperation *>(userp);
  if (nullptr == self)
  {
    return 0;
  }

  self->raw_response_.insert(self->raw_response_.end(), static_cast<char *>(contents),
                             static_cast<char *>(contents) + (size * nmemb));

  if (self->WasAborted())
  {
    return 0;
  }

  if (self->GetSessionState() == opentelemetry::ext::http::client::SessionState::Connecting)
  {
    self->DispatchEvent(opentelemetry::ext::http::client::SessionState::Connected);
  }

  if (self->GetSessionState() == opentelemetry::ext::http::client::SessionState::Connected)
  {
    self->DispatchEvent(opentelemetry::ext::http::client::SessionState::Sending);
  }

  return size * nmemb;
}

size_t HttpOperation::WriteVectorHeaderCallback(void *ptr, size_t size, size_t nmemb, void *userp)
{
  HttpOperation *self = reinterpret_cast<HttpOperation *>(userp);
  if (nullptr == self)
  {
    return 0;
  }

  const unsigned char *begin = static_cast<unsigned char *>(ptr);
  const unsigned char *end   = begin + size * nmemb;
  self->response_headers_.insert(self->response_headers_.end(), begin, end);

  if (self->WasAborted())
  {
    return 0;
  }

  if (self->GetSessionState() == opentelemetry::ext::http::client::SessionState::Connecting)
  {
    self->DispatchEvent(opentelemetry::ext::http::client::SessionState::Connected);
  }

  if (self->GetSessionState() == opentelemetry::ext::http::client::SessionState::Connected)
  {
    self->DispatchEvent(opentelemetry::ext::http::client::SessionState::Sending);
  }

  return size * nmemb;
}

size_t HttpOperation::WriteVectorBodyCallback(void *ptr, size_t size, size_t nmemb, void *userp)
{
  HttpOperation *self = reinterpret_cast<HttpOperation *>(userp);
  if (nullptr == self)
  {
    return 0;
  }

  const unsigned char *begin = static_cast<unsigned char *>(ptr);
  const unsigned char *end   = begin + size * nmemb;
  self->response_body_.insert(self->response_body_.end(), begin, end);

  if (self->WasAborted())
  {
    return 0;
  }

  if (self->GetSessionState() == opentelemetry::ext::http::client::SessionState::Connecting)
  {
    self->DispatchEvent(opentelemetry::ext::http::client::SessionState::Connected);
  }

  if (self->GetSessionState() == opentelemetry::ext::http::client::SessionState::Connected)
  {
    self->DispatchEvent(opentelemetry::ext::http::client::SessionState::Sending);
  }

  return size * nmemb;
}

size_t HttpOperation::ReadMemoryCallback(char *buffer, size_t size, size_t nitems, void *userp)
{
  HttpOperation *self = reinterpret_cast<HttpOperation *>(userp);
  if (nullptr == self)
  {
    return 0;
  }

  if (self->WasAborted())
  {
    return CURL_READFUNC_ABORT;
  }

  if (self->GetSessionState() == opentelemetry::ext::http::client::SessionState::Connecting)
  {
    self->DispatchEvent(opentelemetry::ext::http::client::SessionState::Connected);
  }

  if (self->GetSessionState() == opentelemetry::ext::http::client::SessionState::Connected)
  {
    self->DispatchEvent(opentelemetry::ext::http::client::SessionState::Sending);
  }

  // EOF
  if (self->request_nwrite_ >= self->request_body_.size())
  {
    return 0;
  }

  size_t nwrite = size * nitems;
  if (nwrite > self->request_body_.size() - self->request_nwrite_)
  {
    nwrite = self->request_body_.size() - self->request_nwrite_;
  }

  memcpy(buffer, &self->request_body_[self->request_nwrite_], nwrite);
  self->request_nwrite_ += nwrite;
  return nwrite;
}

#if LIBCURL_VERSION_NUM >= 0x075000
int HttpOperation::PreRequestCallback(void *clientp, char *, char *, int, int)
{
  HttpOperation *self = reinterpret_cast<HttpOperation *>(clientp);
  if (nullptr == self)
  {
    return CURL_PREREQFUNC_ABORT;
  }

  if (self->GetSessionState() == opentelemetry::ext::http::client::SessionState::Connecting)
  {
    self->DispatchEvent(opentelemetry::ext::http::client::SessionState::Connected);
  }

  if (self->WasAborted())
  {
    return CURL_PREREQFUNC_ABORT;
  }

  return CURL_PREREQFUNC_OK;
}
#endif

#if LIBCURL_VERSION_NUM >= 0x072000
int HttpOperation::OnProgressCallback(void *clientp,
                                      curl_off_t /* dltotal */,
                                      curl_off_t /* dlnow */,
                                      curl_off_t /* ultotal */,
                                      curl_off_t /* ulnow */)
{
  HttpOperation *self = reinterpret_cast<HttpOperation *>(clientp);
  if (nullptr == self)
  {
    return -1;
  }

  if (self->WasAborted())
  {
    return -1;
  }

  // CURL_PROGRESSFUNC_CONTINUE is added in 7.68.0
#  if defined(CURL_PROGRESSFUNC_CONTINUE)
  return CURL_PROGRESSFUNC_CONTINUE;
#  else
  return 0;
#  endif
}
#else
int HttpOperation::OnProgressCallback(void *clientp,
                                      double /* dltotal */,
                                      double /* dlnow */,
                                      double /* ultotal */,
                                      double /* ulnow */)
{
  HttpOperation *self = reinterpret_cast<HttpOperation *>(clientp);
  if (nullptr == self)
  {
    return -1;
  }

  if (self->WasAborted())
  {
    return -1;
  }

  return 0;
}
#endif

void HttpOperation::DispatchEvent(opentelemetry::ext::http::client::SessionState type,
                                  const std::string &reason)
{
  if (event_handle_ != nullptr)
  {
    event_handle_->OnEvent(type, reason);
  }

  session_state_ = type;
}

HttpOperation::HttpOperation(opentelemetry::ext::http::client::Method method,
                             std::string url,
                             const HttpSslOptions &ssl_options,
                             opentelemetry::ext::http::client::EventHandler *event_handle,
                             // Default empty headers and empty request body
                             const opentelemetry::ext::http::client::Headers &request_headers,
                             const opentelemetry::ext::http::client::Body &request_body,
                             const opentelemetry::ext::http::client::Compression &compression,
                             // Default connectivity and response size options
                             bool is_raw_response,
                             std::chrono::milliseconds http_conn_timeout,
                             bool reuse_connection,
                             bool is_log_enabled,
                             const RetryPolicy &retry_policy)
    : is_aborted_(false),
      is_finished_(false),
      is_cleaned_(false),
      // Optional connection params
      is_raw_response_(is_raw_response),
      reuse_connection_(reuse_connection),
      http_conn_timeout_(http_conn_timeout),
      // Result
      last_curl_result_(CURLE_OK),
      event_handle_(event_handle),
      method_(method),
      url_(std::move(url)),
      ssl_options_(ssl_options),
      // Local vars
      request_headers_(request_headers),
      request_body_(request_body),
      request_nwrite_(0),
      session_state_(opentelemetry::ext::http::client::SessionState::Created),
      compression_(compression),
      is_log_enabled_(is_log_enabled),
      retry_policy_(retry_policy),
      retry_attempts_((retry_policy.max_attempts > 0U &&
                       retry_policy.initial_backoff > SecondsDecimal::zero() &&
                       retry_policy.max_backoff > SecondsDecimal::zero() &&
                       retry_policy.backoff_multiplier > 0.0f)
                          ? 0
                          : retry_policy.max_attempts),
      response_code_(0)
{
  /* get a curl handle */
  curl_resource_.easy_handle = curl_easy_init();
  if (!curl_resource_.easy_handle)
  {
    last_curl_result_ = CURLE_FAILED_INIT;
    DispatchEvent(opentelemetry::ext::http::client::SessionState::CreateFailed,
                  curl_easy_strerror(last_curl_result_));
    return;
  }

  // Specify our custom headers
  if (!this->request_headers_.empty())
  {
    for (auto &kv : this->request_headers_)
    {
      const auto header = std::string(kv.first).append(": ").append(kv.second);
      curl_resource_.headers_chunk =
          curl_slist_append(curl_resource_.headers_chunk, header.c_str());
    }
  }

  DispatchEvent(opentelemetry::ext::http::client::SessionState::Created);
}

HttpOperation::~HttpOperation()
{
  // Given the request has not been aborted we should wait for completion here
  // This guarantees the lifetime of this request.
  switch (GetSessionState())
  {
    case opentelemetry::ext::http::client::SessionState::Connecting:
    case opentelemetry::ext::http::client::SessionState::Connected:
    case opentelemetry::ext::http::client::SessionState::Sending: {
      if (async_data_ && async_data_->result_future.valid())
      {
        if (HttpOperationAccessor::GetThreadId(*async_data_) != std::this_thread::get_id())
        {
          async_data_->result_future.wait();
          last_curl_result_ = async_data_->result_future.get();
        }
      }
      break;
    }
    default:
      break;
  }

  Cleanup();
}

void HttpOperation::Finish()
{
  if (is_finished_.exchange(true, std::memory_order_acq_rel))
  {
    return;
  }

  if (async_data_ && async_data_->result_future.valid())
  {
    // We should not wait in callback from Cleanup()
    if (HttpOperationAccessor::GetThreadId(*async_data_) != std::this_thread::get_id())
    {
      async_data_->result_future.wait();
      last_curl_result_ = async_data_->result_future.get();
    }
  }
}

void HttpOperation::Cleanup()
{
  if (is_cleaned_.exchange(true, std::memory_order_acq_rel))
  {
    return;
  }

  switch (GetSessionState())
  {
    case opentelemetry::ext::http::client::SessionState::Created:
    case opentelemetry::ext::http::client::SessionState::Connecting:
    case opentelemetry::ext::http::client::SessionState::Connected:
    case opentelemetry::ext::http::client::SessionState::Sending: {
      const char *message = GetCurlErrorMessage(last_curl_result_);
      DispatchEvent(opentelemetry::ext::http::client::SessionState::Cancelled, message);
      break;
    }
    default:
      break;
  }

  std::function<void(HttpOperation &)> callback;

  // Only cleanup async once even in recursive calls
  if (async_data_)
  {
    // Just reset and move easy_handle to owner if in async mode
    if (async_data_->session != nullptr)
    {
      auto session         = async_data_->session;
      async_data_->session = nullptr;

      if (curl_resource_.easy_handle != nullptr)
      {
        curl_easy_setopt(curl_resource_.easy_handle, CURLOPT_PRIVATE, NULL);
        curl_easy_reset(curl_resource_.easy_handle);
      }
      session->GetHttpClient().ScheduleRemoveSession(session->GetSessionId(),
                                                     std::move(curl_resource_));
    }

    callback.swap(async_data_->callback);
    if (callback)
    {
      HttpOperationAccessor::SetThreadId(*async_data_, std::this_thread::get_id());
      callback(*this);
      HttpOperationAccessor::SetThreadId(*async_data_, std::thread::id());
    }

    // Set value to promise to continue Finish()
    if (true == async_data_->is_promise_running.exchange(false, std::memory_order_acq_rel))
    {
      async_data_->result_promise.set_value(last_curl_result_);
    }

    return;
  }

  // Sync mode
  if (curl_resource_.easy_handle != nullptr)
  {
    curl_easy_cleanup(curl_resource_.easy_handle);
    curl_resource_.easy_handle = nullptr;
  }

  if (curl_resource_.headers_chunk != nullptr)
  {
    curl_slist_free_all(curl_resource_.headers_chunk);
    curl_resource_.headers_chunk = nullptr;
  }
}

bool HttpOperation::IsRetryable()
{
#ifdef ENABLE_OTLP_RETRY_PREVIEW
  static constexpr auto kRetryableStatusCodes = std::array<decltype(response_code_), 4>{
      429,  // Too Many Requests
      502,  // Bad Gateway
      503,  // Service Unavailable
      504   // Gateway Timeout
  };

  const auto is_retryable = std::find(kRetryableStatusCodes.cbegin(), kRetryableStatusCodes.cend(),
                                      response_code_) != kRetryableStatusCodes.cend();

  return is_retryable && (last_curl_result_ == CURLE_OK) &&
         (retry_attempts_ < retry_policy_.max_attempts);
#else
  return false;
#endif  // ENABLE_OTLP_RETRY_PREVIEW
}

std::chrono::system_clock::time_point HttpOperation::NextRetryTime()
{
  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_real_distribution<float> dis(0.8f, 1.2f);

  // The initial retry attempt will occur after initialBackoff * random(0.8, 1.2)
  auto backoff = retry_policy_.initial_backoff;

  // After that, the n-th attempt will occur after
  // min(initialBackoff*backoffMultiplier**(n-1), maxBackoff) * random(0.8, 1.2))
  if (retry_attempts_ > 1)
  {
    backoff = (std::min)(retry_policy_.initial_backoff *
                             std::pow(retry_policy_.backoff_multiplier,
                                      static_cast<SecondsDecimal::rep>(retry_attempts_ - 1)),
                         retry_policy_.max_backoff);
  }

  // Jitter of plus or minus 0.2 is applied to the backoff delay to avoid hammering servers at the
  // same time from a large number of clients. Note that this means that the backoff delay may
  // actually be slightly lower than initialBackoff or slightly higher than maxBackoff
  backoff *= dis(gen);

  return last_attempt_time_ + std::chrono::duration_cast<std::chrono::milliseconds>(backoff);
}

/*
  Support for TLS min version, TLS max version.

  To represent versions, the following symbols are needed:

  Added in CURL 7.34.0:
  - CURL_SSLVERSION_TLSv1_0 (do not use)
  - CURL_SSLVERSION_TLSv1_1 (do not use)
  - CURL_SSLVERSION_TLSv1_2

  Added in CURL 7.52.0:
  - CURL_SSLVERSION_TLSv1_3

  Added in CURL 7.54.0:
  - CURL_SSLVERSION_MAX_TLSv1_0 (do not use)
  - CURL_SSLVERSION_MAX_TLSv1_1 (do not use)
  - CURL_SSLVERSION_MAX_TLSv1_2
  - CURL_SSLVERSION_MAX_TLSv1_3

  For 7.34.0 <= CURL < 7.54.0,
  we don't want to get into partial support.

  CURL 7.54.0 is required.
*/
#if LIBCURL_VERSION_NUM >= CURL_VERSION_BITS(7, 54, 0)
#  define HAVE_TLS_VERSION
#endif

// NOLINTNEXTLINE(google-runtime-int)
static long parse_min_ssl_version(const std::string &version)
{
#ifdef HAVE_TLS_VERSION
  if (version == "1.2")
  {
    return CURL_SSLVERSION_TLSv1_2;
  }

  if (version == "1.3")
  {
    return CURL_SSLVERSION_TLSv1_3;
  }
#endif

  return 0;
}

// NOLINTNEXTLINE(google-runtime-int)
static long parse_max_ssl_version(const std::string &version)
{
#ifdef HAVE_TLS_VERSION
  if (version == "1.2")
  {
    return CURL_SSLVERSION_MAX_TLSv1_2;
  }

  if (version == "1.3")
  {
    return CURL_SSLVERSION_MAX_TLSv1_3;
  }
#endif

  return 0;
}

const char *HttpOperation::GetCurlErrorMessage(CURLcode code)
{
  const char *message;

  if (curl_error_message_[0] != '\0')
  {
    // Will contain contextual data
    message = curl_error_message_;
  }
  else
  {
    // Generic message
    message = curl_easy_strerror(code);
  }

  return message;
}

CURLcode HttpOperation::SetCurlPtrOption(CURLoption option, void *value)
{
  CURLcode rc;

  /*
    curl_easy_setopt() is a macro with variadic arguments, type unsafe.
    Various SetCurlXxxOption() helpers ensure it is called with a pointer,
    which can be:
    - a string (const char*)
    - a blob (struct curl_blob*)
    - a headers chunk (curl_slist *)
  */
  rc = curl_easy_setopt(curl_resource_.easy_handle, option, value);

  if (rc != CURLE_OK)
  {
    const char *message = GetCurlErrorMessage(rc);
    OTEL_INTERNAL_LOG_ERROR("CURL, set option <" << std::to_string(option) << "> failed: <"
                                                 << message << ">");
  }

  return rc;
}

// NOLINTNEXTLINE(google-runtime-int)
CURLcode HttpOperation::SetCurlLongOption(CURLoption option, long value)
{
  CURLcode rc;

  /*
    curl_easy_setopt() is a macro with variadic arguments, type unsafe.
    SetCurlLongOption() ensures it is called with a long.
  */
  rc = curl_easy_setopt(curl_resource_.easy_handle, option, value);

  if (rc != CURLE_OK)
  {
    const char *message = GetCurlErrorMessage(rc);
    OTEL_INTERNAL_LOG_ERROR("CURL, set option <" << std::to_string(option) << "> failed: <"
                                                 << message << ">");
  }

  return rc;
}

CURLcode HttpOperation::SetCurlOffOption(CURLoption option, curl_off_t value)
{
  CURLcode rc;

  /*
    curl_easy_setopt() is a macro with variadic arguments, type unsafe.
    SetCurlOffOption() ensures it is called with a curl_off_t.
  */
  rc = curl_easy_setopt(curl_resource_.easy_handle, option, value);

  if (rc != CURLE_OK)
  {
    const char *message = GetCurlErrorMessage(rc);
    OTEL_INTERNAL_LOG_ERROR("CURL, set option <" << std::to_string(option) << "> failed: <"
                                                 << message << ">");
  }

  return rc;
}

int HttpOperation::CurlLoggerCallback(const CURL * /* handle */,
                                      curl_infotype type,
                                      const char *data,
                                      size_t size,
                                      void * /* clientp */) noexcept
{
  nostd::string_view text_to_log{data, size};

  if (!text_to_log.empty() && text_to_log[size - 1] == '\n')
  {
    text_to_log = text_to_log.substr(0, size - 1);
  }

  if (type == CURLINFO_TEXT)
  {
    static const auto kTlsInfo    = nostd::string_view("SSL connection using");
    static const auto kFailureMsg = nostd::string_view("Recv failure:");

    if (text_to_log.substr(0, kTlsInfo.size()) == kTlsInfo)
    {
      OTEL_INTERNAL_LOG_INFO(text_to_log);
    }
    else if (text_to_log.substr(0, kFailureMsg.size()) == kFailureMsg)
    {
      OTEL_INTERNAL_LOG_ERROR(text_to_log);
    }
// This guard serves as a catch-all block for all other less interesting output that should
// remain available for maintainer internal use and for debugging purposes only.
#ifdef OTEL_CURL_DEBUG
    else
    {
      OTEL_INTERNAL_LOG_DEBUG(text_to_log);
    }
#endif  // OTEL_CURL_DEBUG
  }
// Same as above, this guard is meant only for internal use by maintainers, and should not be used
// in production (information leak).
#ifdef OTEL_CURL_DEBUG
  else if (type == CURLINFO_HEADER_OUT)
  {
    static const auto kHeaderSent = nostd::string_view("Send header => ");

    while (!text_to_log.empty() && !std::iscntrl(text_to_log[0]))
    {
      const auto pos = text_to_log.find('\n');

      if (pos != nostd::string_view::npos)
      {
        OTEL_INTERNAL_LOG_DEBUG(kHeaderSent << text_to_log.substr(0, pos - 1));
        text_to_log = text_to_log.substr(pos + 1);
      }
    }
  }
  else if (type == CURLINFO_HEADER_IN)
  {
    static const auto kHeaderRecv = nostd::string_view("Recv header => ");
    OTEL_INTERNAL_LOG_DEBUG(kHeaderRecv << text_to_log);
  }
#endif  // OTEL_CURL_DEBUG

  return 0;
}

CURLcode HttpOperation::Setup()
{
#ifdef ENABLE_CURL_LOGGING
  static constexpr auto kEnableCurlLogging = true;
#else
  static constexpr auto kEnableCurlLogging = false;
#endif  // ENABLE_CURL_LOGGING

  if (!curl_resource_.easy_handle)
  {
    return CURLE_FAILED_INIT;
  }

  CURLcode rc;

  curl_error_message_[0] = '\0';
  curl_easy_setopt(curl_resource_.easy_handle, CURLOPT_ERRORBUFFER, curl_error_message_);

// Support for custom debug function callback was added in version 7.9.6 so we guard against
// exposing the default CURL output by keeping verbosity always disabled in lower versions.
#if LIBCURL_VERSION_NUM < CURL_VERSION_BITS(7, 9, 6)
  rc = SetCurlLongOption(CURLOPT_VERBOSE, 0L);
  if (rc != CURLE_OK)
  {
    return rc;
  }
#else
  rc = SetCurlLongOption(CURLOPT_VERBOSE, (is_log_enabled_ && kEnableCurlLogging) ? 1L : 0L);
  if (rc != CURLE_OK)
  {
    return rc;
  }

  rc = SetCurlPtrOption(CURLOPT_DEBUGFUNCTION,
                        reinterpret_cast<void *>(&HttpOperation::CurlLoggerCallback));
  if (rc != CURLE_OK)
  {
    return rc;
  }
#endif

  // Specify target URL
  rc = SetCurlStrOption(CURLOPT_URL, url_.c_str());
  if (rc != CURLE_OK)
  {
    return rc;
  }

  if (ssl_options_.use_ssl)
  {
    /* 1 - CA CERT */

    if (!ssl_options_.ssl_ca_cert_path.empty())
    {
      const char *path = ssl_options_.ssl_ca_cert_path.c_str();

      rc = SetCurlStrOption(CURLOPT_CAINFO, path);
      if (rc != CURLE_OK)
      {
        return rc;
      }
    }
    else if (!ssl_options_.ssl_ca_cert_string.empty())
    {
#if LIBCURL_VERSION_NUM >= CURL_VERSION_BITS(7, 77, 0)
      const char *data = ssl_options_.ssl_ca_cert_string.c_str();
      size_t data_len  = ssl_options_.ssl_ca_cert_string.length();

      struct curl_blob stblob;
      stblob.data  = const_cast<char *>(data);
      stblob.len   = data_len;
      stblob.flags = CURL_BLOB_COPY;

      rc = SetCurlBlobOption(CURLOPT_CAINFO_BLOB, &stblob);
      if (rc != CURLE_OK)
      {
        return rc;
      }
#else
      // CURL 7.77.0 required for CURLOPT_CAINFO_BLOB.
      OTEL_INTERNAL_LOG_ERROR("CURL 7.77.0 required for CA CERT STRING");
      return CURLE_UNKNOWN_OPTION;
#endif
    }

    /* 2 - CLIENT KEY */

    if (!ssl_options_.ssl_client_key_path.empty())
    {
      const char *path = ssl_options_.ssl_client_key_path.c_str();

      rc = SetCurlStrOption(CURLOPT_SSLKEY, path);
      if (rc != CURLE_OK)
      {
        return rc;
      }

      rc = SetCurlStrOption(CURLOPT_SSLKEYTYPE, "PEM");
      if (rc != CURLE_OK)
      {
        return rc;
      }
    }
    else if (!ssl_options_.ssl_client_key_string.empty())
    {
#if LIBCURL_VERSION_NUM >= CURL_VERSION_BITS(7, 71, 0)
      const char *data = ssl_options_.ssl_client_key_string.c_str();
      size_t data_len  = ssl_options_.ssl_client_key_string.length();

      struct curl_blob stblob;
      stblob.data  = const_cast<char *>(data);
      stblob.len   = data_len;
      stblob.flags = CURL_BLOB_COPY;

      rc = SetCurlBlobOption(CURLOPT_SSLKEY_BLOB, &stblob);
      if (rc != CURLE_OK)
      {
        return rc;
      }

      rc = SetCurlStrOption(CURLOPT_SSLKEYTYPE, "PEM");
      if (rc != CURLE_OK)
      {
        return rc;
      }
#else
      // CURL 7.71.0 required for CURLOPT_SSLKEY_BLOB.
      OTEL_INTERNAL_LOG_ERROR("CURL 7.71.0 required for CLIENT KEY STRING");
      return CURLE_UNKNOWN_OPTION;
#endif
    }

    /* 3 - CLIENT CERT */

    if (!ssl_options_.ssl_client_cert_path.empty())
    {
      const char *path = ssl_options_.ssl_client_cert_path.c_str();

      rc = SetCurlStrOption(CURLOPT_SSLCERT, path);
      if (rc != CURLE_OK)
      {
        return rc;
      }

      rc = SetCurlStrOption(CURLOPT_SSLCERTTYPE, "PEM");
      if (rc != CURLE_OK)
      {
        return rc;
      }
    }
    else if (!ssl_options_.ssl_client_cert_string.empty())
    {
#if LIBCURL_VERSION_NUM >= CURL_VERSION_BITS(7, 71, 0)
      const char *data = ssl_options_.ssl_client_cert_string.c_str();
      size_t data_len  = ssl_options_.ssl_client_cert_string.length();

      struct curl_blob stblob;
      stblob.data  = const_cast<char *>(data);
      stblob.len   = data_len;
      stblob.flags = CURL_BLOB_COPY;

      rc = SetCurlBlobOption(CURLOPT_SSLCERT_BLOB, &stblob);
      if (rc != CURLE_OK)
      {
        return rc;
      }

      rc = SetCurlStrOption(CURLOPT_SSLCERTTYPE, "PEM");
      if (rc != CURLE_OK)
      {
        return rc;
      }
#else
      // CURL 7.71.0 required for CURLOPT_SSLCERT_BLOB.
      OTEL_INTERNAL_LOG_ERROR("CURL 7.71.0 required for CLIENT CERT STRING");
      return CURLE_UNKNOWN_OPTION;
#endif
    }

    /* 4 - TLS */

#ifdef HAVE_TLS_VERSION
    /* By default, TLSv1.2 or better is required (if we have TLS). */
    // NOLINTNEXTLINE(google-runtime-int)
    long min_ssl_version = CURL_SSLVERSION_TLSv1_2;
#else
    // NOLINTNEXTLINE(google-runtime-int)
    long min_ssl_version = 0;
#endif

    if (!ssl_options_.ssl_min_tls.empty())
    {
#ifdef HAVE_TLS_VERSION
      min_ssl_version = parse_min_ssl_version(ssl_options_.ssl_min_tls);

      if (min_ssl_version == 0)
      {
        OTEL_INTERNAL_LOG_ERROR("Unknown min TLS version <" << ssl_options_.ssl_min_tls << ">");
        return CURLE_UNKNOWN_OPTION;
      }
#else
      OTEL_INTERNAL_LOG_ERROR("CURL 7.54.0 required for MIN TLS");
      return CURLE_UNKNOWN_OPTION;
#endif
    }

    /*
     * Do not set a max TLS version by default.
     * The CURL + openssl library may be more recent than this code,
     * and support a version we do not know about.
     */
    // NOLINTNEXTLINE(google-runtime-int)
    long max_ssl_version = 0;

    if (!ssl_options_.ssl_max_tls.empty())
    {
#ifdef HAVE_TLS_VERSION
      max_ssl_version = parse_max_ssl_version(ssl_options_.ssl_max_tls);

      if (max_ssl_version == 0)
      {
        OTEL_INTERNAL_LOG_ERROR("Unknown max TLS version <" << ssl_options_.ssl_max_tls << ">");
        return CURLE_UNKNOWN_OPTION;
      }
#else
      OTEL_INTERNAL_LOG_ERROR("CURL 7.54.0 required for MAX TLS");
      return CURLE_UNKNOWN_OPTION;
#endif
    }

    // NOLINTNEXTLINE(google-runtime-int)
    long version_range = min_ssl_version | max_ssl_version;
    if (version_range != 0)
    {
      rc = SetCurlLongOption(CURLOPT_SSLVERSION, version_range);
      if (rc != CURLE_OK)
      {
        return rc;
      }
    }

    /* 5 - CIPHER */

    if (!ssl_options_.ssl_cipher.empty())
    {
      /* TLS 1.2 */
      const char *cipher_list = ssl_options_.ssl_cipher.c_str();

      rc = SetCurlStrOption(CURLOPT_SSL_CIPHER_LIST, cipher_list);
      if (rc != CURLE_OK)
      {
        return rc;
      }
    }

    if (!ssl_options_.ssl_cipher_suite.empty())
    {
#if LIBCURL_VERSION_NUM >= CURL_VERSION_BITS(7, 61, 0)
      /* TLS 1.3 */
      const char *cipher_list = ssl_options_.ssl_cipher_suite.c_str();

      rc = SetCurlStrOption(CURLOPT_TLS13_CIPHERS, cipher_list);
#else
      // CURL 7.61.0 required for CURLOPT_TLS13_CIPHERS.
      OTEL_INTERNAL_LOG_ERROR("CURL 7.61.0 required for CIPHER SUITE");
      return CURLE_UNKNOWN_OPTION;
#endif

      if (rc != CURLE_OK)
      {
        return rc;
      }
    }

    if (ssl_options_.ssl_insecure_skip_verify)
    {
      /* 6 - DO NOT ENFORCE VERIFICATION, This is not secure. */
      // NOLINTNEXTLINE(google-runtime-int)
      rc = SetCurlLongOption(CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_NONE));
      if (rc != CURLE_OK)
      {
        return rc;
      }

      rc = SetCurlLongOption(CURLOPT_SSL_VERIFYPEER, 0L);
      if (rc != CURLE_OK)
      {
        return rc;
      }

      rc = SetCurlLongOption(CURLOPT_SSL_VERIFYHOST, 0L);
      if (rc != CURLE_OK)
      {
        return rc;
      }
    }
    else
    {
      /* 6 - ENFORCE VERIFICATION */
      // NOLINTNEXTLINE(google-runtime-int)
      rc = SetCurlLongOption(CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
      if (rc != CURLE_OK)
      {
        return rc;
      }

      rc = SetCurlLongOption(CURLOPT_SSL_VERIFYPEER, 1L);
      if (rc != CURLE_OK)
      {
        return rc;
      }

      rc = SetCurlLongOption(CURLOPT_SSL_VERIFYHOST, 2L);
      if (rc != CURLE_OK)
      {
        return rc;
      }
    }
  }
  else
  {
    rc = SetCurlLongOption(CURLOPT_SSL_VERIFYPEER, 0L);
    if (rc != CURLE_OK)
    {
      return rc;
    }

    rc = SetCurlLongOption(CURLOPT_SSL_VERIFYHOST, 0L);
    if (rc != CURLE_OK)
    {
      return rc;
    }
  }

  if (compression_ == opentelemetry::ext::http::client::Compression::kGzip)
  {
    rc = SetCurlStrOption(CURLOPT_ACCEPT_ENCODING, "gzip");
    if (rc != CURLE_OK)
    {
      return rc;
    }
  }

  if (curl_resource_.headers_chunk != nullptr)
  {
    rc = SetCurlListOption(CURLOPT_HTTPHEADER, curl_resource_.headers_chunk);
    if (rc != CURLE_OK)
    {
      return rc;
    }
  }

  // TODO: control local port to use
  // curl_easy_setopt(curl, CURLOPT_LOCALPORT, dcf_port);
  // NOLINTNEXTLINE(google-runtime-int)
  rc = SetCurlLongOption(CURLOPT_TIMEOUT_MS, static_cast<long>(http_conn_timeout_.count()));
  if (rc != CURLE_OK)
  {
    return rc;
  }

  // abort if slower than 4kb/sec during 30 seconds
  rc = SetCurlLongOption(CURLOPT_LOW_SPEED_TIME, 30L);
  if (rc != CURLE_OK)
  {
    return rc;
  }

  rc = SetCurlLongOption(CURLOPT_LOW_SPEED_LIMIT, 4096L);
  if (rc != CURLE_OK)
  {
    return rc;
  }

  if (reuse_connection_)
  {
    rc = SetCurlLongOption(CURLOPT_FRESH_CONNECT, 0L);
    if (rc != CURLE_OK)
    {
      return rc;
    }

    rc = SetCurlLongOption(CURLOPT_FORBID_REUSE, 0L);
    if (rc != CURLE_OK)
    {
      return rc;
    }
  }
  else
  {
    rc = SetCurlLongOption(CURLOPT_FRESH_CONNECT, 1L);
    if (rc != CURLE_OK)
    {
      return rc;
    }
    rc = SetCurlLongOption(CURLOPT_FORBID_REUSE, 1L);
    if (rc != CURLE_OK)
    {
      return rc;
    }
  }

  if (is_raw_response_)
  {
    rc = SetCurlLongOption(CURLOPT_HEADER, 1L);
    if (rc != CURLE_OK)
    {
      return rc;
    }

    rc = SetCurlPtrOption(CURLOPT_WRITEFUNCTION,
                          reinterpret_cast<void *>(&HttpOperation::WriteMemoryCallback));
    if (rc != CURLE_OK)
    {
      return rc;
    }

    rc = SetCurlPtrOption(CURLOPT_WRITEDATA, this);
    if (rc != CURLE_OK)
    {
      return rc;
    }
  }
  else
  {
    rc = SetCurlPtrOption(CURLOPT_WRITEFUNCTION,
                          reinterpret_cast<void *>(&HttpOperation::WriteVectorBodyCallback));
    if (rc != CURLE_OK)
    {
      return rc;
    }

    rc = SetCurlPtrOption(CURLOPT_WRITEDATA, this);
    if (rc != CURLE_OK)
    {
      return rc;
    }

    rc = SetCurlPtrOption(CURLOPT_HEADERFUNCTION,
                          reinterpret_cast<void *>(&HttpOperation::WriteVectorHeaderCallback));
    if (rc != CURLE_OK)
    {
      return rc;
    }

    rc = SetCurlPtrOption(CURLOPT_HEADERDATA, this);
    if (rc != CURLE_OK)
    {
      return rc;
    }
  }

  // TODO: only two methods supported for now - POST and GET
  if (method_ == opentelemetry::ext::http::client::Method::Post)
  {
    // Request buffer
    const curl_off_t req_size = static_cast<curl_off_t>(request_body_.size());
    // POST
    rc = SetCurlLongOption(CURLOPT_POST, 1L);
    if (rc != CURLE_OK)
    {
      return rc;
    }

    rc = SetCurlStrOption(CURLOPT_POSTFIELDS, NULL);
    if (rc != CURLE_OK)
    {
      return rc;
    }

    rc = SetCurlOffOption(CURLOPT_POSTFIELDSIZE_LARGE, req_size);
    if (rc != CURLE_OK)
    {
      return rc;
    }

    rc = SetCurlPtrOption(CURLOPT_READFUNCTION,
                          reinterpret_cast<void *>(&HttpOperation::ReadMemoryCallback));
    if (rc != CURLE_OK)
    {
      return rc;
    }

    rc = SetCurlPtrOption(CURLOPT_READDATA, this);
    if (rc != CURLE_OK)
    {
      return rc;
    }
  }
  else if (method_ == opentelemetry::ext::http::client::Method::Get)
  {
    // GET
  }
  else
  {
    OTEL_INTERNAL_LOG_ERROR("Unexpected HTTP method");
    return CURLE_UNSUPPORTED_PROTOCOL;
  }

#if LIBCURL_VERSION_NUM >= 0x072000
  rc = SetCurlPtrOption(CURLOPT_XFERINFOFUNCTION,
                        reinterpret_cast<void *>(&HttpOperation::OnProgressCallback));
  if (rc != CURLE_OK)
  {
    return rc;
  }

  rc = SetCurlPtrOption(CURLOPT_XFERINFODATA, this);
  if (rc != CURLE_OK)
  {
    return rc;
  }
#else
  rc = SetCurlPtrOption(CURLOPT_PROGRESSFUNCTION, (void *)&HttpOperation::OnProgressCallback);
  if (rc != CURLE_OK)
  {
    return rc;
  }

  rc = SetCurlPtrOption(CURLOPT_PROGRESSDATA, this);
  if (rc != CURLE_OK)
  {
    return rc;
  }
#endif

#if LIBCURL_VERSION_NUM >= 0x075000
  rc = SetCurlPtrOption(CURLOPT_PREREQFUNCTION,
                        reinterpret_cast<void *>(&HttpOperation::PreRequestCallback));
  if (rc != CURLE_OK)
  {
    return rc;
  }

  rc = SetCurlPtrOption(CURLOPT_PREREQDATA, this);
  if (rc != CURLE_OK)
  {
    return rc;
  }
#endif

  return CURLE_OK;
}

CURLcode HttpOperation::Send()
{
  // If it is async sending, just return error
  if (async_data_ && async_data_->is_promise_running.load(std::memory_order_acquire))
  {
    return CURLE_FAILED_INIT;
  }

  ReleaseResponse();

  last_curl_result_ = Setup();
  if (last_curl_result_ != CURLE_OK)
  {
    const char *message = GetCurlErrorMessage(last_curl_result_);
    DispatchEvent(opentelemetry::ext::http::client::SessionState::ConnectFailed, message);
    return last_curl_result_;
  }

  // Perform initial connect, handling the timeout if needed
  // We can not use CURLOPT_CONNECT_ONLY because it will disable the reuse of connections.
  DispatchEvent(opentelemetry::ext::http::client::SessionState::Connecting);
  is_finished_.store(false, std::memory_order_release);
  is_aborted_.store(false, std::memory_order_release);
  is_cleaned_.store(false, std::memory_order_release);

  CURLcode code = curl_easy_perform(curl_resource_.easy_handle);
  PerformCurlMessage(code);
  return code;
}

CURLcode HttpOperation::SendAsync(Session *session, std::function<void(HttpOperation &)> callback)
{
  if (nullptr == session)
  {
    return CURLE_FAILED_INIT;
  }

  if (async_data_ && async_data_->is_promise_running.load(std::memory_order_acquire))
  {
    return CURLE_FAILED_INIT;
  }

  async_data_.reset(new AsyncData());
  async_data_->is_promise_running.store(false, std::memory_order_release);
  async_data_->session = nullptr;

  ReleaseResponse();

  CURLcode code     = Setup();
  last_curl_result_ = code;
  if (code != CURLE_OK)
  {
    const char *message = GetCurlErrorMessage(code);
    DispatchEvent(opentelemetry::ext::http::client::SessionState::ConnectFailed, message);
    return code;
  }
  curl_easy_setopt(curl_resource_.easy_handle, CURLOPT_PRIVATE, session);

  DispatchEvent(opentelemetry::ext::http::client::SessionState::Connecting);
  is_finished_.store(false, std::memory_order_release);
  is_aborted_.store(false, std::memory_order_release);
  is_cleaned_.store(false, std::memory_order_release);

  async_data_->session = session;
  if (false == async_data_->is_promise_running.exchange(true, std::memory_order_acq_rel))
  {
    async_data_->result_promise = std::promise<CURLcode>();
    async_data_->result_future  = async_data_->result_promise.get_future();
  }
  async_data_->callback = std::move(callback);

  session->GetHttpClient().ScheduleAddSession(session->GetSessionId());
  return CURLE_OK;
}

Headers HttpOperation::GetResponseHeaders()
{
  Headers result;
  if (response_headers_.size() == 0)
    return result;

  std::stringstream ss;
  std::string headers(reinterpret_cast<const char *>(&response_headers_[0]),
                      response_headers_.size());
  ss.str(headers);

  std::string header;
  while (std::getline(ss, header, '\n'))
  {
    // TODO - Regex below crashes with out-of-memory on CI docker container, so
    // switching to string comparison. Need to debug and revert back.

    /*std::smatch match;
    std::regex http_headers_regex(kHttpHeaderRegexp);
    if (std::regex_search(header, match, http_headers_regex))
      result.insert(std::pair<nostd::string_view, nostd::string_view>(
          static_cast<nostd::string_view>(match[1]), static_cast<nostd::string_view>(match[2])));
    */
    size_t pos = header.find(": ");
    if (pos != std::string::npos)
      result.insert(
          std::pair<std::string, std::string>(header.substr(0, pos), header.substr(pos + 2)));
  }
  return result;
}

void HttpOperation::ReleaseResponse()
{
  response_headers_.clear();
  response_body_.clear();
  raw_response_.clear();
}

void HttpOperation::Abort()
{
  is_aborted_.store(true, std::memory_order_release);
  if (curl_resource_.easy_handle != nullptr)
  {
    // Enable progress callback to abort from polling thread
    curl_easy_setopt(curl_resource_.easy_handle, CURLOPT_NOPROGRESS, 0L);
    if (async_data_ && nullptr != async_data_->session)
    {
      async_data_->session->GetHttpClient().ScheduleAbortSession(
          async_data_->session->GetSessionId());
    }
  }
}

void HttpOperation::PerformCurlMessage(CURLcode code)
{
  ++retry_attempts_;
  last_attempt_time_ = std::chrono::system_clock::now();
  last_curl_result_  = code;

  if (code != CURLE_OK)
  {
    switch (GetSessionState())
    {
      case opentelemetry::ext::http::client::SessionState::Connecting: {
        const char *message = GetCurlErrorMessage(code);
        DispatchEvent(opentelemetry::ext::http::client::SessionState::ConnectFailed,
                      message);  // couldn't connect - stage 1
        break;
      }
      case opentelemetry::ext::http::client::SessionState::Connected:
      case opentelemetry::ext::http::client::SessionState::Sending: {
        if (GetSessionState() == opentelemetry::ext::http::client::SessionState::Connected)
        {
          DispatchEvent(opentelemetry::ext::http::client::SessionState::Sending);
        }

        const char *message = GetCurlErrorMessage(code);
        DispatchEvent(opentelemetry::ext::http::client::SessionState::SendFailed, message);
      }
      default:
        break;
    }
  }
  else if (curl_resource_.easy_handle != nullptr)
  {
    curl_easy_getinfo(curl_resource_.easy_handle, CURLINFO_RESPONSE_CODE, &response_code_);
  }

  // Transform state
  if (GetSessionState() == opentelemetry::ext::http::client::SessionState::Connecting)
  {
    DispatchEvent(opentelemetry::ext::http::client::SessionState::Connected);
  }

  if (GetSessionState() == opentelemetry::ext::http::client::SessionState::Connected)
  {
    DispatchEvent(opentelemetry::ext::http::client::SessionState::Sending);
  }

  if (GetSessionState() == opentelemetry::ext::http::client::SessionState::Sending)
  {
    DispatchEvent(opentelemetry::ext::http::client::SessionState::Response);
  }

  if (IsRetryable())
  {
    // Clear any response data received in previous attempt
    ReleaseResponse();
    // Rewind request data so that read callback can re-transfer the payload
    request_nwrite_ = 0;
    // Reset session state
    DispatchEvent(opentelemetry::ext::http::client::SessionState::Connecting);
  }
  else
  {
    // Cleanup and unbind easy handle from multi handle, and finish callback
    Cleanup();
  }
}

}  // namespace curl
}  // namespace client
}  // namespace http
}  // namespace ext
OPENTELEMETRY_END_NAMESPACE
