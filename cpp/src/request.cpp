#include "request.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <new>
#include <system_error>
#include <utility>

namespace catbox::detail {
namespace {

namespace fs = std::filesystem;

class CurlGlobal {
public:
    CurlGlobal() {
        if (const CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT); code != CURLE_OK) {
            throw Error(Error::Kind::network, std::string("curl_global_init failed: ") + curl_easy_strerror(code));
        }
    }
    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;
    ~CurlGlobal() { curl_global_cleanup(); }
};

void check(CURLcode code, std::string_view what) {
    if (code != CURLE_OK) {
        throw Error(Error::Kind::network, std::string(what) + ": " + curl_easy_strerror(code));
    }
}

std::string to_utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

// Callbacks handed to libcurl. They must not let exceptions escape into C code.

std::size_t read_file(char* buffer, std::size_t size, std::size_t count, void* stream) noexcept {
    auto& file = *static_cast<std::ifstream*>(stream);
    file.read(buffer, static_cast<std::streamsize>(size * count));
    if (file.bad()) {
        return CURL_READFUNC_ABORT;
    }
    return static_cast<std::size_t>(file.gcount());
}

int seek_file(void* stream, curl_off_t offset, int origin) noexcept {
    // curl rewinds the body when it has to resend it, e.g. after a redirect or an auth challenge.
    auto& file = *static_cast<std::ifstream*>(stream);
    file.clear();
    const auto direction = origin == SEEK_CUR ? std::ios::cur : origin == SEEK_END ? std::ios::end : std::ios::beg;
    file.seekg(static_cast<std::streamoff>(offset), direction);
    return file.fail() ? CURL_SEEKFUNC_FAIL : CURL_SEEKFUNC_OK;
}

std::size_t append_body(char* data, std::size_t size, std::size_t count, void* body) noexcept {
    try {
        static_cast<std::string*>(body)->append(data, size * count);
        return size * count;
    } catch (const std::bad_alloc&) {
        return 0;  // anything other than the full length makes curl fail the transfer
    }
}

struct ProgressState {
    const ProgressCallback* callback;
    std::exception_ptr error;
};

int report_progress(void* state_pointer, curl_off_t /*download_total*/, curl_off_t /*downloaded*/,
                    curl_off_t upload_total, curl_off_t uploaded) noexcept {
    auto& state = *static_cast<ProgressState*>(state_pointer);
    if (upload_total <= 0) {
        return 0;
    }
    try {
        (*state.callback)(static_cast<std::uint64_t>(uploaded), static_cast<std::uint64_t>(upload_total));
        return 0;
    } catch (...) {
        state.error = std::current_exception();  // rethrown after curl_easy_perform returns
        return 1;                                // aborts the transfer
    }
}

constexpr bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
    while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
    return text;
}

}  // namespace

void ensure_curl_initialized() {
    static const CurlGlobal guard;
}

Request::Request(std::string endpoint) : endpoint_(std::move(endpoint)) {
    ensure_curl_initialized();
    easy_.reset(curl_easy_init());
    if (!easy_) {
        throw Error(Error::Kind::network, "curl_easy_init failed");
    }
    mime_.reset(curl_mime_init(easy_.get()));
    if (!mime_) {
        throw Error(Error::Kind::network, "curl_mime_init failed");
    }
}

curl_mimepart* Request::add_part(std::string_view name) {
    curl_mimepart* part = curl_mime_addpart(mime_.get());
    if (part == nullptr) {
        throw std::bad_alloc();
    }
    check(curl_mime_name(part, std::string(name).c_str()), "curl_mime_name");
    return part;
}

void Request::add_field(std::string_view name, std::string_view value) {
    check(curl_mime_data(add_part(name), value.data(), value.size()), "curl_mime_data");
}

void Request::add_file(std::string_view name, const fs::path& path) {
    std::error_code error;
    const fs::file_status status = fs::status(path, error);
    if (status.type() == fs::file_type::not_found) {
        throw Error(Error::Kind::file, "no such file");
    }
    if (error) {
        throw Error(Error::Kind::file, error.message());
    }
    if (fs::is_directory(status)) {
        throw Error(Error::Kind::file, "is a directory");
    }
    // A regular file has a known size, which lets curl send Content-Length instead of chunking.
    if (!fs::is_regular_file(status)) {
        throw Error(Error::Kind::file, "not a regular file");
    }
    const std::uintmax_t size = fs::file_size(path, error);
    if (error) {
        throw Error(Error::Kind::file, error.message());
    }
    // std::ifstream takes the path natively (UTF-16 on Windows), so any file name opens.
    auto& file = files_.emplace_back(std::make_unique<std::ifstream>(path, std::ios::binary));
    if (!file->is_open()) {
        throw Error(Error::Kind::file, "cannot open file for reading");
    }

    const std::string filename = to_utf8(path.filename());
    curl_mimepart* part = add_part(name);
    check(curl_mime_data_cb(part, static_cast<curl_off_t>(size), read_file, seek_file, nullptr, file.get()),
          "curl_mime_data_cb");
    check(curl_mime_filename(part, filename.c_str()), "curl_mime_filename");
    check(curl_mime_type(part, std::string(mime_type(filename)).c_str()), "curl_mime_type");
}

void Request::add_data(std::string_view name, std::string_view filename, std::span<const std::byte> data) {
    curl_mimepart* part = add_part(name);
    check(curl_mime_data(part, reinterpret_cast<const char*>(data.data()), data.size()), "curl_mime_data");
    check(curl_mime_filename(part, std::string(filename).c_str()), "curl_mime_filename");
    check(curl_mime_type(part, std::string(mime_type(filename)).c_str()), "curl_mime_type");
}

std::string Request::send(const ProgressCallback& progress) {
    CURL* easy = easy_.get();
    std::string body;
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    ProgressState progress_state{&progress, nullptr};

    check(curl_easy_setopt(easy, CURLOPT_URL, endpoint_.c_str()), "CURLOPT_URL");
    check(curl_easy_setopt(easy, CURLOPT_MIMEPOST, mime_.get()), "CURLOPT_MIMEPOST");
    const std::string user_agent = "catbox-cpp/" + std::string(version);
    check(curl_easy_setopt(easy, CURLOPT_USERAGENT, user_agent.c_str()), "CURLOPT_USERAGENT");
    check(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L), "CURLOPT_NOSIGNAL");
    check(curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, error_buffer.data()), "CURLOPT_ERRORBUFFER");
    check(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, append_body), "CURLOPT_WRITEFUNCTION");
    check(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &body), "CURLOPT_WRITEDATA");
    if (progress) {
        check(curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, report_progress), "CURLOPT_XFERINFOFUNCTION");
        check(curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &progress_state), "CURLOPT_XFERINFODATA");
        check(curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L), "CURLOPT_NOPROGRESS");
    }

    const CURLcode code = curl_easy_perform(easy);
    if (progress_state.error) {
        std::rethrow_exception(progress_state.error);
    }
    if (code != CURLE_OK) {
        throw Error(Error::Kind::network, error_buffer[0] != '\0' ? error_buffer.data() : curl_easy_strerror(code));
    }
    long status = 0;
    check(curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status), "CURLINFO_RESPONSE_CODE");
    return parse_response(status, body);
}

std::string parse_response(long status, std::string_view body) {
    std::string text(trim(body));
    if (status < 200 || status >= 300) {
        std::string message = "server returned HTTP " + std::to_string(status);
        if (!text.empty()) {
            message += ": " + text;
        }
        throw Error(Error::Kind::http_status, message, static_cast<int>(status), std::move(text));
    }
    // Catbox reports most errors as plain text with status 200.
    if (!text.starts_with("https://") && !text.starts_with("http://")) {
        std::string message = text.empty() ? "server returned an empty response" : "server error: " + text;
        throw Error(Error::Kind::unexpected_response, message, static_cast<int>(status), std::move(text));
    }
    return text;
}

}  // namespace catbox::detail
