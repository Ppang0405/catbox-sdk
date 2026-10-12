// Unit tests for the library: MIME lookup, expiry parsing, response parsing and the checks
// that run before any request is sent. No network access beyond a refused localhost port.
#include "request.hpp"

#include <catbox/catbox.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, std::string_view what, std::source_location where = std::source_location::current()) {
    if (!condition) {
        ++failures;
        std::cerr << where.file_name() << ':' << where.line() << ": check failed: " << what << '\n';
    }
}

#define CHECK(condition) check((condition), #condition)

/// Runs `action`, expecting a catbox::Error, and returns it.
catbox::Error expect_error(const std::function<void()>& action,
                           std::source_location where = std::source_location::current()) {
    try {
        action();
    } catch (const catbox::Error& error) {
        return error;
    }
    check(false, "expected catbox::Error", where);
    return catbox::Error(catbox::Error::Kind::network, "no error");
}

void test_mime_type() {
    CHECK(catbox::mime_type("photo.jpg") == "image/jpeg");
    CHECK(catbox::mime_type("UPPER.JPG") == "image/jpeg");
    CHECK(catbox::mime_type("a.JpEg") == "image/jpeg");
    CHECK(catbox::mime_type("archive.tar.gz") == "application/gzip");
    CHECK(catbox::mime_type("dir.png/notes.txt") == "text/plain");
    CHECK(catbox::mime_type("C:\\pics\\cat.webp") == "image/webp");
    CHECK(catbox::mime_type("a.mp4") == "video/mp4");
    CHECK(catbox::mime_type("a.7z") == "application/x-7z-compressed");
    CHECK(catbox::mime_type("noextension") == "application/octet-stream");
    CHECK(catbox::mime_type("data.unknownext") == "application/octet-stream");
    CHECK(catbox::mime_type(".png") == "application/octet-stream");  // a dotfile, not an extension
    CHECK(catbox::mime_type("..png") == "application/octet-stream");
    CHECK(catbox::mime_type("trailing.") == "application/octet-stream");
    CHECK(catbox::mime_type("") == "application/octet-stream");
}

void test_expiry() {
    for (const auto expiry : {catbox::Expiry::one_hour, catbox::Expiry::twelve_hours, catbox::Expiry::one_day,
                              catbox::Expiry::three_days}) {
        CHECK(catbox::parse_expiry(catbox::to_string(expiry)) == expiry);
    }
    CHECK(catbox::to_string(catbox::Expiry::twelve_hours) == "12h");
    CHECK(!catbox::parse_expiry("5h"));
    CHECK(!catbox::parse_expiry("1H"));
    CHECK(!catbox::parse_expiry(""));
}

void test_parse_response() {
    using catbox::detail::parse_response;
    using Kind = catbox::Error::Kind;

    CHECK(parse_response(200, "  https://files.catbox.moe/abc.png\n") == "https://files.catbox.moe/abc.png");
    CHECK(parse_response(201, "http://example.com/x") == "http://example.com/x");

    const auto rejected = expect_error([] { (void)parse_response(200, "No files given.\n"); });
    CHECK(rejected.kind() == Kind::unexpected_response);
    CHECK(rejected.status() == 200);
    CHECK(rejected.server_message() == "No files given.");
    CHECK(std::string_view(rejected.what()) == "server error: No files given.");

    const auto empty = expect_error([] { (void)parse_response(200, " \r\n"); });
    CHECK(std::string_view(empty.what()) == "server returned an empty response");

    const auto failed = expect_error([] { (void)parse_response(500, "Internal Server Error"); });
    CHECK(failed.kind() == Kind::http_status);
    CHECK(failed.status() == 500);
    CHECK(std::string_view(failed.what()) == "server returned HTTP 500: Internal Server Error");

    // A URL is still a failure when the status says so.
    const auto redirected = expect_error([] { (void)parse_response(302, "https://catbox.moe/"); });
    CHECK(redirected.kind() == Kind::http_status);
    CHECK(std::string_view(expect_error([] { (void)parse_response(404, ""); }).what()) == "server returned HTTP 404");
}

class TemporaryDirectory {
public:
    TemporaryDirectory() : path_(std::filesystem::temp_directory_path() / "catbox-cpp-tests") {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void test_local_file_errors() {
    using Kind = catbox::Error::Kind;
    const TemporaryDirectory directory;
    // Nothing listens here; a file check that wrongly passed would surface as a network error.
    const catbox::CatboxClient client(catbox::Endpoint("http://127.0.0.1:9/user/api.php"));
    const catbox::LitterboxClient litterbox(catbox::Endpoint("http://127.0.0.1:9/resources/internals/api.php"));

    const auto missing = expect_error([&] { (void)client.upload_file(directory.path() / "missing.png"); });
    CHECK(missing.kind() == Kind::file);
    CHECK(std::string_view(missing.what()) == "no such file");
    CHECK(!missing.status());

    const auto is_directory = expect_error([&] { (void)litterbox.upload_file(directory.path()); });
    CHECK(is_directory.kind() == Kind::file);
    CHECK(std::string_view(is_directory.what()) == "is a directory");
}

void test_network_error() {
    const TemporaryDirectory directory;
    const auto file = directory.path() / "a.txt";
    std::ofstream(file) << "hello";

    const catbox::CatboxClient client(catbox::UserHash("HASH"), catbox::Endpoint("http://127.0.0.1:9/user/api.php"));
    const auto refused = expect_error([&] { (void)client.upload_file(file); });
    CHECK(refused.kind() == catbox::Error::Kind::network);
    CHECK(!refused.status());
    CHECK(!std::string_view(refused.what()).empty());

    const auto url = expect_error([&] { (void)client.upload_url("https://example.com/a.png"); });
    CHECK(url.kind() == catbox::Error::Kind::network);

    const std::string text = "in memory";
    const auto data = expect_error([&] { (void)client.upload_data(std::as_bytes(std::span(text)), "notes.txt"); });
    CHECK(data.kind() == catbox::Error::Kind::network);
}

void test_clients() {
    const catbox::CatboxClient anonymous;
    CHECK(anonymous.user_hash().empty());
    CHECK(anonymous.endpoint().url() == catbox::catbox_endpoint);

    const catbox::CatboxClient mine(catbox::UserHash("abc123"));
    CHECK(mine.user_hash().value() == "abc123");

    const catbox::CatboxClient local(catbox::Endpoint("http://127.0.0.1:8080/user/api.php"));
    CHECK(local.user_hash().empty());
    CHECK(local.endpoint().url() == "http://127.0.0.1:8080/user/api.php");

    CHECK(catbox::LitterboxClient().endpoint().url() == catbox::litterbox_endpoint);
    CHECK(catbox::version == "0.1.0");
}

}  // namespace

int main() {
    test_mime_type();
    test_clients();
    test_expiry();
    test_parse_response();
    test_local_file_errors();
    test_network_error();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
