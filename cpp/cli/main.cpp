// The `catbox` command. See conformance/SPEC.md for the behavior shared by every implementation.
#include "terminal.hpp"

#include <catbox/catbox.hpp>

#include <CLI/CLI.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int exit_failure = 1;
constexpr int exit_usage = 2;

using UploadFn = std::function<std::string(const std::filesystem::path&, const catbox::ProgressCallback&)>;

/// Command-line arguments arrive as UTF-8 (CLI11's ensure_utf8 converts them on Windows).
/// Constructing the path from char8_t keeps non-ASCII names intact there.
std::filesystem::path path_from_utf8(const std::string& text) {
    return {std::u8string(text.begin(), text.end())};
}

std::string utf8_filename(const std::filesystem::path& path) {
    const std::u8string name = path.filename().u8string();
    return {name.begin(), name.end()};
}

std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
    // MSVC deprecates std::getenv; _dupenv_s hands back a malloc'd copy we must free.
    char* raw = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&raw, &length, name) != 0 || raw == nullptr) {
        return std::nullopt;
    }
    const std::unique_ptr<char, decltype(&std::free)> value(raw, &std::free);
    return std::string(value.get());
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
#endif
}

void print_url(const std::string& url) {
    std::cout << url << '\n' << std::flush;  // flush per line so pipes see each URL as it arrives
}

/// Uploads each file in turn, printing one URL per line. Returns false if any upload failed.
bool upload_each(const std::vector<std::string>& files, bool quiet, const UploadFn& upload) {
    const bool show_progress = !quiet && terminal::stderr_is_interactive();
    bool ok = true;
    for (const std::string& argument : files) {
        const std::filesystem::path path = path_from_utf8(argument);
        try {
            std::string url;
            {
                // Scoped so the bar erases itself before the URL or error is printed.
                std::optional<terminal::ProgressBar> bar;
                catbox::ProgressCallback progress;
                if (show_progress) {
                    bar.emplace(utf8_filename(path));
                    progress = [&bar](std::uint64_t sent, std::uint64_t total) { bar->update(sent, total); };
                }
                url = upload(path, progress);
            }
            print_url(url);
        } catch (const std::exception& error) {
            ok = false;
            terminal::report_error(argument, error.what());
        }
    }
    return ok;
}

bool upload_urls(const std::vector<std::string>& urls, const catbox::CatboxClient& client) {
    bool ok = true;
    for (const std::string& url : urls) {
        try {
            print_url(client.upload_url(url));
        } catch (const std::exception& error) {
            ok = false;
            terminal::report_error(url, error.what());
        }
    }
    return ok;
}

struct Arguments {
    std::optional<std::string> user_hash;
    std::optional<std::string> endpoint;
    bool quiet = false;
    std::string time = std::string(catbox::to_string(catbox::Expiry::three_days));
    std::vector<std::string> items;
};

catbox::UserHash user_hash(const Arguments& arguments) {
    return catbox::UserHash(arguments.user_hash ? *arguments.user_hash
                                                : environment("CATBOX_USERHASH").value_or(""));
}

catbox::Endpoint endpoint(const Arguments& arguments, std::string_view fallback) {
    return catbox::Endpoint(arguments.endpoint ? std::string_view(*arguments.endpoint) : fallback);
}

void add_user_hash_option(CLI::App& command, Arguments& arguments) {
    command.add_option("--user-hash", arguments.user_hash,
                       "Your catbox user hash, so uploads show up in your account (default: $CATBOX_USERHASH)");
}

void add_common_options(CLI::App& command, Arguments& arguments, bool with_quiet) {
    if (with_quiet) {
        command.add_flag("-q,--quiet", arguments.quiet, "Don't show upload progress");
    }
    // For tests against a local server; the empty group hides it from --help.
    command.add_option("--endpoint", arguments.endpoint, "Override the API URL")->group("");
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"Upload files to catbox.moe and litterbox.catbox.moe.", "catbox"};
    argv = app.ensure_utf8(argv);
    app.set_version_flag("--version", std::string(catbox::version));
    app.require_subcommand(1);

    Arguments arguments;

    CLI::App* upload = app.add_subcommand("upload", "Upload files to catbox.moe permanently (max 200 MB each)");
    add_user_hash_option(*upload, arguments);
    add_common_options(*upload, arguments, true);
    upload->add_option("FILE", arguments.items, "Files to upload")->required();

    CLI::App* upload_url = app.add_subcommand("upload-url", "Have catbox.moe fetch and store files from remote URLs");
    add_user_hash_option(*upload_url, arguments);
    add_common_options(*upload_url, arguments, false);
    upload_url->add_option("URL", arguments.items, "URLs to copy to catbox")->required();

    CLI::App* litter = app.add_subcommand("litter", "Upload files to litterbox.catbox.moe temporarily (max 1 GB each)");
    const CLI::Validator expiry(
        [](const std::string& value) {
            return catbox::parse_expiry(value) ? std::string() : "must be one of 1h, 12h, 24h or 72h";
        },
        "");
    litter->add_option("-t,--time", arguments.time, "How long to keep the files: 1h, 12h, 24h or 72h")
        ->type_name("TIME")
        ->check(expiry)
        ->capture_default_str();
    add_common_options(*litter, arguments, true);
    litter->add_option("FILE", arguments.items, "Files to upload")->required();

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        // --help and --version arrive here too, with exit code 0. CLI11 uses codes above 100 for
        // usage errors; map them to 2, the conventional usage-error status.
        return app.exit(error) == 0 ? EXIT_SUCCESS : exit_usage;
    }

    bool ok = true;
    if (upload->parsed()) {
        const catbox::CatboxClient client(user_hash(arguments), endpoint(arguments, catbox::catbox_endpoint));
        ok = upload_each(arguments.items, arguments.quiet,
                         [&client](const std::filesystem::path& path, const catbox::ProgressCallback& progress) {
                             return client.upload_file(path, progress);
                         });
    } else if (upload_url->parsed()) {
        const catbox::CatboxClient client(user_hash(arguments), endpoint(arguments, catbox::catbox_endpoint));
        ok = upload_urls(arguments.items, client);
    } else if (litter->parsed()) {
        const catbox::LitterboxClient client(endpoint(arguments, catbox::litterbox_endpoint));
        const catbox::Expiry time = catbox::parse_expiry(arguments.time).value();  // validated by CLI11
        ok = upload_each(arguments.items, arguments.quiet,
                         [&client, time](const std::filesystem::path& path, const catbox::ProgressCallback& progress) {
                             return client.upload_file(path, time, progress);
                         });
    }
    return ok ? EXIT_SUCCESS : exit_failure;
}
