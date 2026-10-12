// RAII ownership of libcurl objects. Internal to the library.
#pragma once

#include <curl/curl.h>

#include <memory>

namespace catbox::detail {

/// Runs curl_global_init once per process, before the first handle is created, and
/// curl_global_cleanup at exit. A function-local static makes the first call thread-safe.
void ensure_curl_initialized();

struct EasyDeleter {
    void operator()(CURL* handle) const noexcept { curl_easy_cleanup(handle); }
};
struct MimeDeleter {
    void operator()(curl_mime* mime) const noexcept { curl_mime_free(mime); }
};

using EasyHandle = std::unique_ptr<CURL, EasyDeleter>;
using MimeHandle = std::unique_ptr<curl_mime, MimeDeleter>;

}  // namespace catbox::detail
