/*
 * Unit tests: MIME lookup, base names, response parsing, argument parsing and
 * the library's argument and file checks. No network beyond 127.0.0.1.
 * conformance/run.py covers the real HTTP exchange.
 */
#include <stdio.h>
#include <string.h>

#include "catbox.h"
#include "catbox_internal.h"
#include "cli.h"
#include "progress.h"

static int failures;

static void check(int ok, const char *what, const char *file, int line)
{
    if (!ok) {
        failures++;
        fprintf(stderr, "%s:%d: check failed: %s\n", file, line, what);
    }
}

static void check_string(const char *actual, const char *expected, const char *what, const char *file, int line)
{
    if (actual == NULL || strcmp(actual, expected) != 0) {
        failures++;
        fprintf(stderr, "%s:%d: %s was \"%s\", expected \"%s\"\n", file, line, what, actual ? actual : "(null)",
                expected);
    }
}

#define CHECK(condition) check((condition) != 0, #condition, __FILE__, __LINE__)
#define CHECK_STRING(actual, expected) check_string((actual), (expected), #actual, __FILE__, __LINE__)

/* ---- library helpers ---------------------------------------------------- */

static void test_mime_type(void)
{
    CHECK_STRING(catbox_mime_type("photo.jpg"), "image/jpeg");
    CHECK_STRING(catbox_mime_type("photo.jpeg"), "image/jpeg");
    CHECK_STRING(catbox_mime_type("UPPER.JPG"), "image/jpeg");
    CHECK_STRING(catbox_mime_type("a.png"), "image/png");
    CHECK_STRING(catbox_mime_type("a.gif"), "image/gif");
    CHECK_STRING(catbox_mime_type("a.webp"), "image/webp");
    CHECK_STRING(catbox_mime_type("notes.txt"), "text/plain");
    CHECK_STRING(catbox_mime_type("a.json"), "application/json");
    CHECK_STRING(catbox_mime_type("a.pdf"), "application/pdf");
    CHECK_STRING(catbox_mime_type("a.zip"), "application/zip");
    CHECK_STRING(catbox_mime_type("clip.Mp4"), "video/mp4");
    CHECK_STRING(catbox_mime_type("logo.svg"), "image/svg+xml");
    CHECK_STRING(catbox_mime_type("backup.tar.gz"), "application/gzip");
    CHECK_STRING(catbox_mime_type("dir/sub/x.7z"), "application/x-7z-compressed");
    /* No extension, unknown extension, or only leading/trailing dots. */
    CHECK_STRING(catbox_mime_type("noextension"), "application/octet-stream");
    CHECK_STRING(catbox_mime_type("data.unknownext"), "application/octet-stream");
    CHECK_STRING(catbox_mime_type(".txt"), "application/octet-stream");
    CHECK_STRING(catbox_mime_type("..png"), "application/octet-stream");
    CHECK_STRING(catbox_mime_type("trailing."), "application/octet-stream");
    CHECK_STRING(catbox_mime_type("dir.png/file"), "application/octet-stream");
    CHECK_STRING(catbox_mime_type(""), "application/octet-stream");
}

static void test_basename(void)
{
    CHECK_STRING(catbox_path_basename("photo.jpg"), "photo.jpg");
    CHECK_STRING(catbox_path_basename("nested/dir/my photo.png"), "my photo.png");
    CHECK_STRING(catbox_path_basename("/abs/path.txt"), "path.txt");
    CHECK_STRING(catbox_path_basename("dir/"), "");
    CHECK_STRING(catbox_path_basename(""), "");
    CHECK_STRING(catbox_path_basename(NULL), "");
#ifdef _WIN32
    CHECK_STRING(catbox_path_basename("C:\\Users\\me\\photo.jpg"), "photo.jpg");
    CHECK_STRING(catbox_path_basename("C:photo.jpg"), "photo.jpg");
    CHECK_STRING(catbox_path_basename("mixed/dir\\file.txt"), "file.txt");
    CHECK_STRING(catbox_path_basename("file.txt:stream"), "file.txt:stream");
#else
    /* A backslash is an ordinary file name character on POSIX systems. */
    CHECK_STRING(catbox_path_basename("dir/back\\slash.txt"), "back\\slash.txt");
#endif
}

static void test_expiry(void)
{
    catbox_expiry expiry = CATBOX_EXPIRY_1H;
    CHECK(catbox_expiry_parse("12h", &expiry) && expiry == CATBOX_EXPIRY_12H);
    CHECK(catbox_expiry_parse("72h", &expiry) && expiry == CATBOX_EXPIRY_72H);
    CHECK(!catbox_expiry_parse("5h", &expiry));
    CHECK(!catbox_expiry_parse("1H", &expiry));
    CHECK(!catbox_expiry_parse("", &expiry));
    CHECK(!catbox_expiry_parse(NULL, &expiry));
    CHECK_STRING(catbox_expiry_name(CATBOX_EXPIRY_1H), "1h");
    CHECK_STRING(catbox_expiry_name(CATBOX_EXPIRY_24H), "24h");
    CHECK(catbox_expiry_name((catbox_expiry)42) == NULL);
}

/* ---- response parsing --------------------------------------------------- */

static char *parse(long status, const char *body, catbox_error *error)
{
    return catbox_parse_response(status, body, body ? strlen(body) : 0, error);
}

static void test_parse_response(void)
{
    catbox_error error;
    char *url;

    url = parse(200, "  https://files.catbox.moe/abc123.png\r\n", &error);
    CHECK_STRING(url, "https://files.catbox.moe/abc123.png");
    CHECK(error.code == CATBOX_OK && error.http_status == 200 && error.message[0] == '\0');
    catbox_free(url);

    url = parse(201, "http://example.test/x", &error);
    CHECK_STRING(url, "http://example.test/x");
    catbox_free(url);

    /* Catbox's usual way of failing: status 200 with plain text. */
    CHECK(parse(200, "No files given.\n", &error) == NULL);
    CHECK(error.code == CATBOX_ERR_SERVER && error.http_status == 200);
    CHECK_STRING(error.message, "server error: No files given.");

    CHECK(parse(200, " \n", &error) == NULL);
    CHECK_STRING(error.message, "server returned an empty response");
    CHECK(parse(200, NULL, &error) == NULL);
    CHECK(error.code == CATBOX_ERR_SERVER);

    /* "httpsfoo" and a URL that is not at the start are not URLs. */
    CHECK(parse(200, "httpsfoo", &error) == NULL);
    CHECK(parse(200, "see https://files.catbox.moe/x", &error) == NULL);

    CHECK(parse(500, "Internal Server Error", &error) == NULL);
    CHECK(error.code == CATBOX_ERR_HTTP_STATUS && error.http_status == 500);
    CHECK_STRING(error.message, "server returned HTTP 500: Internal Server Error");

    /* A URL-looking body does not rescue an error status. */
    CHECK(parse(404, "https://files.catbox.moe/x", &error) == NULL);
    CHECK(error.code == CATBOX_ERR_HTTP_STATUS);

    CHECK(parse(503, "", &error) == NULL);
    CHECK_STRING(error.message, "server returned HTTP 503");

    /* The error argument is optional. */
    CHECK(parse(500, "x", NULL) == NULL);

    /* Bodies need not be NUL-terminated. */
    url = catbox_parse_response(200, "https://a.test/1XXXX", 17, &error);
    CHECK_STRING(url, "https://a.test/1X");
    catbox_free(url);
}

/* ---- library argument and file checks ----------------------------------- */

static void test_upload_checks(void)
{
    catbox_options options = {0};
    catbox_error error;

    /* Port 1 on localhost: nothing should be listening, and these all fail
     * before connecting anyway. */
    options.endpoint = "http://127.0.0.1:1/user/api.php";

    CHECK(catbox_upload_file(&options, "definitely/missing/file.png", &error) == NULL);
    CHECK(error.code == CATBOX_ERR_FILE && error.http_status == 0);
    CHECK_STRING(error.message, "no such file");

    CHECK(catbox_upload_file(&options, ".", &error) == NULL);
    CHECK(error.code == CATBOX_ERR_FILE);
    CHECK_STRING(error.message, "is a directory");

    CHECK(catbox_litter_upload_file(&options, ".", CATBOX_EXPIRY_1H, &error) == NULL);
    CHECK_STRING(error.message, "is a directory");

    CHECK(catbox_upload_file(&options, NULL, &error) == NULL);
    CHECK(error.code == CATBOX_ERR_INVALID_ARGUMENT);
    CHECK(catbox_upload_file(&options, "", &error) == NULL);
    CHECK(error.code == CATBOX_ERR_INVALID_ARGUMENT);
    CHECK(catbox_upload_url(&options, "", &error) == NULL);
    CHECK(error.code == CATBOX_ERR_INVALID_ARGUMENT);
    CHECK(catbox_upload_buffer(&options, NULL, 10, "a.txt", &error) == NULL);
    CHECK(error.code == CATBOX_ERR_INVALID_ARGUMENT);
    CHECK(catbox_upload_buffer(&options, "x", 1, "", &error) == NULL);
    CHECK(error.code == CATBOX_ERR_INVALID_ARGUMENT);
    CHECK(catbox_litter_upload_file(&options, "a.txt", (catbox_expiry)42, &error) == NULL);
    CHECK(error.code == CATBOX_ERR_INVALID_ARGUMENT);
    CHECK(catbox_upload_file(NULL, NULL, NULL) == NULL);
}

static void test_network_errors(void)
{
    catbox_options options = {0};
    catbox_error error;

    /* Only http and https endpoints are allowed. */
    options.endpoint = "file:///etc/passwd";
    CHECK(catbox_upload_url(&options, "https://example.com/a.png", &error) == NULL);
    CHECK(error.code == CATBOX_ERR_NETWORK && error.http_status == 0 && error.message[0] != '\0');

    options.endpoint = "http://127.0.0.1:1/user/api.php";
    CHECK(catbox_upload_buffer(&options, "hello", 5, "hello.txt", &error) == NULL);
    CHECK(error.code == CATBOX_ERR_NETWORK && error.http_status == 0 && error.message[0] != '\0');
}

/* ---- progress bar helpers ----------------------------------------------- */

static void test_format_bytes(void)
{
    char text[32];
    format_bytes(text, sizeof text, 0);
    CHECK_STRING(text, "0 B");
    format_bytes(text, sizeof text, 1023);
    CHECK_STRING(text, "1023 B");
    format_bytes(text, sizeof text, 1536);
    CHECK_STRING(text, "1.5 KB");
    format_bytes(text, sizeof text, 24 * 1024 * 1024 + 7);
    CHECK_STRING(text, "24.0 MB");
    format_bytes(text, sizeof text, (int64_t)3 << 40);
    CHECK_STRING(text, "3072.0 GB");
}

/* ---- argument parsing --------------------------------------------------- */

#define ARGC(argv) ((int)(sizeof(argv) / sizeof((argv)[0])))

static cli_action parse_args(int argc, char **argv, cli_args *args)
{
    cli_args_free(args);
    cli_parse(argc, argv, args);
    return args->action;
}

static void test_cli_top_level(void)
{
    cli_args args = {0};
    char *none[] = {"catbox"};
    char *help[] = {"catbox", "--help"};
    char *short_help[] = {"catbox", "-h"};
    char *version[] = {"catbox", "--version"};
    char *unknown_command[] = {"catbox", "download", "a.txt"};
    char *unknown_option[] = {"catbox", "--verbose"};

    CHECK(parse_args(ARGC(none), none, &args) == CLI_USAGE_ERROR);
    CHECK(parse_args(ARGC(help), help, &args) == CLI_SHOW_HELP && args.command == CLI_NO_COMMAND);
    CHECK(parse_args(ARGC(short_help), short_help, &args) == CLI_SHOW_HELP);
    CHECK(parse_args(ARGC(version), version, &args) == CLI_SHOW_VERSION);
    CHECK(parse_args(ARGC(unknown_command), unknown_command, &args) == CLI_USAGE_ERROR);
    CHECK(strstr(args.error, "download") != NULL);
    CHECK(parse_args(ARGC(unknown_option), unknown_option, &args) == CLI_USAGE_ERROR);
    cli_args_free(&args);
}

static void test_cli_upload(void)
{
    cli_args args = {0};
    char *basic[] = {"catbox", "upload", "a.png", "-q", "--user-hash", "H1", "b.txt"};
    char *equals[] = {"catbox", "upload", "--user-hash=H2", "--endpoint=http://x/", "--quiet", "a"};
    char *dashdash[] = {"catbox", "upload", "--", "-q", "--odd"};
    char *dash[] = {"catbox", "upload", "-"};
    char *no_files[] = {"catbox", "upload", "-q"};
    char *missing_value[] = {"catbox", "upload", "a.png", "--user-hash"};
    char *unknown[] = {"catbox", "upload", "--no-such-option", "a.txt"};
    char *flag_value[] = {"catbox", "upload", "--quiet=yes", "a.txt"};
    char *bundle[] = {"catbox", "upload", "-qx", "a.txt"};
    char *time_on_upload[] = {"catbox", "upload", "-t", "1h", "a.txt"};
    char *sub_help[] = {"catbox", "upload", "--help"};
    char *empty_hash[] = {"catbox", "upload", "--user-hash", "", "a"};

    CHECK(parse_args(ARGC(basic), basic, &args) == CLI_RUN);
    CHECK(args.command == CLI_UPLOAD && args.quiet && args.endpoint == NULL);
    CHECK_STRING(args.user_hash, "H1");
    CHECK(args.input_count == 2);
    CHECK_STRING(args.inputs[0], "a.png");
    CHECK_STRING(args.inputs[1], "b.txt");

    CHECK(parse_args(ARGC(equals), equals, &args) == CLI_RUN);
    CHECK_STRING(args.user_hash, "H2");
    CHECK_STRING(args.endpoint, "http://x/");
    CHECK(args.quiet && args.input_count == 1);

    CHECK(parse_args(ARGC(dashdash), dashdash, &args) == CLI_RUN);
    CHECK(!args.quiet && args.input_count == 2);
    CHECK_STRING(args.inputs[0], "-q");
    CHECK_STRING(args.inputs[1], "--odd");

    CHECK(parse_args(ARGC(dash), dash, &args) == CLI_RUN && args.input_count == 1);
    CHECK(parse_args(ARGC(empty_hash), empty_hash, &args) == CLI_RUN);
    CHECK_STRING(args.user_hash, "");

    CHECK(parse_args(ARGC(no_files), no_files, &args) == CLI_USAGE_ERROR);
    CHECK_STRING(args.error, "no files given");
    CHECK(parse_args(ARGC(missing_value), missing_value, &args) == CLI_USAGE_ERROR);
    CHECK_STRING(args.error, "option '--user-hash' needs a value");
    CHECK(parse_args(ARGC(unknown), unknown, &args) == CLI_USAGE_ERROR);
    CHECK_STRING(args.error, "unknown option '--no-such-option'");
    CHECK(parse_args(ARGC(flag_value), flag_value, &args) == CLI_USAGE_ERROR);
    CHECK(parse_args(ARGC(bundle), bundle, &args) == CLI_USAGE_ERROR);
    CHECK(parse_args(ARGC(time_on_upload), time_on_upload, &args) == CLI_USAGE_ERROR);
    CHECK(parse_args(ARGC(sub_help), sub_help, &args) == CLI_SHOW_HELP && args.command == CLI_UPLOAD);
    cli_args_free(&args);
}

static void test_cli_upload_url(void)
{
    cli_args args = {0};
    char *basic[] = {"catbox", "upload-url", "--user-hash", "H", "https://a.test/x.png", "https://b.test/y"};
    char *no_urls[] = {"catbox", "upload-url"};

    CHECK(parse_args(ARGC(basic), basic, &args) == CLI_RUN);
    CHECK(args.command == CLI_UPLOAD_URL && args.input_count == 2);
    CHECK_STRING(args.user_hash, "H");
    CHECK(parse_args(ARGC(no_urls), no_urls, &args) == CLI_USAGE_ERROR);
    CHECK_STRING(args.error, "no URLs given");
    cli_args_free(&args);
}

static void test_cli_litter(void)
{
    cli_args args = {0};
    char *defaults[] = {"catbox", "litter", "a.zip"};
    char *short_time[] = {"catbox", "litter", "-t", "1h", "a.zip"};
    char *attached[] = {"catbox", "litter", "-t12h", "a.zip"};
    char *long_time[] = {"catbox", "litter", "a.zip", "--time=24h"};
    char *invalid[] = {"catbox", "litter", "-t", "5h", "a.zip"};
    char *user_hash[] = {"catbox", "litter", "--user-hash", "H", "a.zip"};

    CHECK(parse_args(ARGC(defaults), defaults, &args) == CLI_RUN);
    CHECK(args.command == CLI_LITTER && args.expiry == CATBOX_EXPIRY_72H);
    CHECK(parse_args(ARGC(short_time), short_time, &args) == CLI_RUN && args.expiry == CATBOX_EXPIRY_1H);
    CHECK(parse_args(ARGC(attached), attached, &args) == CLI_RUN && args.expiry == CATBOX_EXPIRY_12H);
    CHECK(parse_args(ARGC(long_time), long_time, &args) == CLI_RUN && args.expiry == CATBOX_EXPIRY_24H);
    CHECK(args.input_count == 1);
    CHECK(parse_args(ARGC(invalid), invalid, &args) == CLI_USAGE_ERROR);
    CHECK(strstr(args.error, "5h") != NULL);
    /* litter has no accounts, so --user-hash is not one of its options. */
    CHECK(parse_args(ARGC(user_hash), user_hash, &args) == CLI_USAGE_ERROR);
    cli_args_free(&args);
}

/* ---- runner ------------------------------------------------------------- */

static const struct {
    const char *name;
    void (*run)(void);
} tests[] = {
    {"mime_type", test_mime_type},
    {"basename", test_basename},
    {"expiry", test_expiry},
    {"parse_response", test_parse_response},
    {"upload_checks", test_upload_checks},
    {"network_errors", test_network_errors},
    {"format_bytes", test_format_bytes},
    {"cli_top_level", test_cli_top_level},
    {"cli_upload", test_cli_upload},
    {"cli_upload_url", test_cli_upload_url},
    {"cli_litter", test_cli_litter},
};

int main(void)
{
    size_t i;
    int failed_tests = 0;

    if (catbox_global_init() != CATBOX_OK) {
        fputs("catbox_global_init failed\n", stderr);
        return 1;
    }
    for (i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = failures;
        tests[i].run();
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", tests[i].name);
        failed_tests += failures != before;
    }
    catbox_global_cleanup();
    printf("\n%d of %d tests failed\n", failed_tests, (int)(sizeof tests / sizeof tests[0]));
    return failed_tests == 0 ? 0 : 1;
}
