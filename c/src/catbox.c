/*
 * catbox.c - the library: request building with curl_mime, response parsing,
 * and the extension -> MIME type table.
 */
#ifndef _WIN32
#  define _POSIX_C_SOURCE 200809L /* stat() and S_ISREG under strict -std=c11 */
#endif

#include "catbox.h"
#include "catbox_internal.h"

#include <curl/curl.h>

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define USER_AGENT "catbox-c/" CATBOX_VERSION

/* Catbox answers with a URL or a one-line error; anything past this is noise. */
#define MAX_RESPONSE_SIZE (64 * 1024)

/* MSVC has no S_ISDIR/S_ISREG. */
#ifdef _WIN32
#  define IS_DIRECTORY(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#  define IS_REGULAR(mode) (((mode) & _S_IFMT) == _S_IFREG)
#else
#  define IS_DIRECTORY(mode) S_ISDIR(mode)
#  define IS_REGULAR(mode) S_ISREG(mode)
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define PRINTF_LIKE(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#  define PRINTF_LIKE(fmt, args)
#endif

/* ---- errors ------------------------------------------------------------- */

static void clear_error(catbox_error *error, long http_status)
{
    if (error) {
        error->code = CATBOX_OK;
        error->http_status = http_status;
        error->message[0] = '\0';
    }
}

PRINTF_LIKE(4, 5)
static void set_error(catbox_error *error, catbox_code code, long http_status, const char *format, ...)
{
    va_list args;
    if (!error)
        return;
    error->code = code;
    error->http_status = http_status;
    va_start(args, format);
    /* Truncation is fine: the message is for people, not for parsing. */
    vsnprintf(error->message, sizeof error->message, format, args);
    va_end(args);
}

/* ---- small string helpers ----------------------------------------------- */

static char *copy_string(const char *text, size_t length)
{
    char *copy = malloc(length + 1);
    if (copy) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

static int equals_ignoring_case(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    }
    return *a == *b;
}

static int has_prefix(const char *text, size_t length, const char *prefix)
{
    size_t prefix_length = strlen(prefix);
    return length >= prefix_length && memcmp(text, prefix, prefix_length) == 0;
}

static int is_empty(const char *text)
{
    return text == NULL || text[0] == '\0';
}

/* ---- paths and MIME types ----------------------------------------------- */

const char *catbox_path_basename(const char *path)
{
    const char *base = path;
    const char *p;
    if (!path)
        return "";
    for (p = path; *p; p++) {
#ifdef _WIN32
        /* Both separators work on Windows, and "C:name" is relative to drive C. */
        if (*p == '/' || *p == '\\' || (*p == ':' && p == path + 1))
            base = p + 1;
#else
        if (*p == '/')
            base = p + 1;
#endif
    }
    return base;
}

/* A fixed table rather than the platform's MIME database, so every OS sends
 * the same Content-Type. Matches the Python reference implementation. */
static const struct {
    const char *extension;
    const char *type;
} mime_types[] = {
    {"7z", "application/x-7z-compressed"},
    {"aac", "audio/aac"},
    {"avif", "image/avif"},
    {"bmp", "image/bmp"},
    {"css", "text/css"},
    {"csv", "text/csv"},
    {"flac", "audio/flac"},
    {"gif", "image/gif"},
    {"gz", "application/gzip"},
    {"heic", "image/heic"},
    {"htm", "text/html"},
    {"html", "text/html"},
    {"ico", "image/vnd.microsoft.icon"},
    {"jpeg", "image/jpeg"},
    {"jpg", "image/jpeg"},
    {"js", "text/javascript"},
    {"json", "application/json"},
    {"m4a", "audio/mp4"},
    {"md", "text/markdown"},
    {"mkv", "video/x-matroska"},
    {"mov", "video/quicktime"},
    {"mp3", "audio/mpeg"},
    {"mp4", "video/mp4"},
    {"ogg", "audio/ogg"},
    {"opus", "audio/opus"},
    {"pdf", "application/pdf"},
    {"png", "image/png"},
    {"svg", "image/svg+xml"},
    {"tar", "application/x-tar"},
    {"tif", "image/tiff"},
    {"tiff", "image/tiff"},
    {"txt", "text/plain"},
    {"wav", "audio/wav"},
    {"webm", "video/webm"},
    {"webp", "image/webp"},
    {"xml", "application/xml"},
    {"zip", "application/zip"},
};

const char *catbox_mime_type(const char *filename)
{
    const char *name = catbox_path_basename(filename);
    const char *dot;
    size_t i;

    /* Like Python's os.path.splitext: leading dots (".bashrc") don't start an
     * extension, and only the last one counts ("a.tar.gz" -> "gz"). */
    while (*name == '.')
        name++;
    dot = strrchr(name, '.');
    if (dot && dot[1] != '\0') {
        for (i = 0; i < sizeof mime_types / sizeof mime_types[0]; i++) {
            if (equals_ignoring_case(dot + 1, mime_types[i].extension))
                return mime_types[i].type;
        }
    }
    return "application/octet-stream";
}

/* ---- expiry ------------------------------------------------------------- */

const char *catbox_expiry_name(catbox_expiry expiry)
{
    switch (expiry) {
    case CATBOX_EXPIRY_1H: return "1h";
    case CATBOX_EXPIRY_12H: return "12h";
    case CATBOX_EXPIRY_24H: return "24h";
    case CATBOX_EXPIRY_72H: return "72h";
    }
    return NULL;
}

int catbox_expiry_parse(const char *name, catbox_expiry *expiry)
{
    static const catbox_expiry all[] = {CATBOX_EXPIRY_1H, CATBOX_EXPIRY_12H, CATBOX_EXPIRY_24H, CATBOX_EXPIRY_72H};
    size_t i;
    if (!name)
        return 0;
    for (i = 0; i < sizeof all / sizeof all[0]; i++) {
        if (strcmp(name, catbox_expiry_name(all[i])) == 0) {
            if (expiry)
                *expiry = all[i];
            return 1;
        }
    }
    return 0;
}

/* ---- responses ---------------------------------------------------------- */

char *catbox_parse_response(long status, const char *body, size_t size, catbox_error *error)
{
    const char *start = body ? body : "";
    const char *end = start + (body ? size : 0);
    int length;
    char *url;

    while (start < end && isspace((unsigned char)*start))
        start++;
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    length = (int)(end - start); /* at most MAX_RESPONSE_SIZE */

    if (status < 200 || status > 299) {
        if (length > 0)
            set_error(error, CATBOX_ERR_HTTP_STATUS, status, "server returned HTTP %ld: %.*s", status, length, start);
        else
            set_error(error, CATBOX_ERR_HTTP_STATUS, status, "server returned HTTP %ld", status);
        return NULL;
    }
    /* Catbox reports most errors as plain text with status 200. */
    if (!has_prefix(start, (size_t)length, "https://") && !has_prefix(start, (size_t)length, "http://")) {
        if (length > 0)
            set_error(error, CATBOX_ERR_SERVER, status, "server error: %.*s", length, start);
        else
            set_error(error, CATBOX_ERR_SERVER, status, "server returned an empty response");
        return NULL;
    }
    url = copy_string(start, (size_t)length);
    if (!url) {
        set_error(error, CATBOX_ERR_OUT_OF_MEMORY, status, "out of memory");
        return NULL;
    }
    clear_error(error, status);
    return url;
}

/* ---- requests ----------------------------------------------------------- */

/* A buffer handed to curl_mime_data_cb, so in-memory uploads aren't copied. */
typedef struct buffer_reader {
    const char *data;
    size_t size;
    size_t offset;
} buffer_reader;

/* Everything one POST needs. Text fields left NULL are not sent. */
typedef struct upload_request {
    const char *endpoint;
    const char *reqtype;
    const char *user_hash; /* sent only when non-empty */
    const char *time;
    const char *url;
    /* Optional fileToUpload part: streamed from file_path, or read from buffer. */
    const char *file_path;
    buffer_reader *buffer;
    const char *file_name;
} upload_request;

typedef struct response_body {
    char *data;
    size_t size;
} response_body;

static size_t read_buffer(char *destination, size_t size, size_t count, void *user_data)
{
    buffer_reader *reader = user_data;
    size_t wanted = size * count;
    size_t left = reader->size - reader->offset;
    size_t n = wanted < left ? wanted : left;
    memcpy(destination, reader->data + reader->offset, n);
    reader->offset += n;
    return n;
}

/* curl rewinds the body when it has to send it again (e.g. after a redirect). */
static int seek_buffer(void *user_data, curl_off_t offset, int origin)
{
    buffer_reader *reader = user_data;
    if (origin != SEEK_SET || offset < 0 || (curl_off_t)reader->size < offset)
        return CURL_SEEKFUNC_FAIL;
    reader->offset = (size_t)offset;
    return CURL_SEEKFUNC_OK;
}

static size_t collect_response(char *data, size_t size, size_t count, void *user_data)
{
    response_body *body = user_data;
    size_t received = size * count;
    size_t room = MAX_RESPONSE_SIZE - body->size;
    size_t kept = received < room ? received : room;
    if (kept > 0) {
        char *grown = realloc(body->data, body->size + kept);
        if (!grown)
            return 0; /* makes curl fail with CURLE_WRITE_ERROR */
        memcpy(grown + body->size, data, kept);
        body->data = grown;
        body->size += kept;
    }
    return received; /* accept, and drop, anything past the limit */
}

static int report_progress(void *user_data, curl_off_t download_total, curl_off_t downloaded, curl_off_t upload_total,
                           curl_off_t uploaded)
{
    const catbox_options *options = user_data;
    (void)download_total;
    (void)downloaded;
    /* Normalize to 0/1: other values have special meanings to curl. */
    return options->progress(options->progress_data, (int64_t)uploaded, (int64_t)upload_total) != 0;
}

static CURLcode add_field(curl_mime *form, const char *name, const char *value)
{
    curl_mimepart *part;
    CURLcode result;
    if (!value)
        return CURLE_OK;
    part = curl_mime_addpart(form);
    if (!part)
        return CURLE_OUT_OF_MEMORY;
    result = curl_mime_name(part, name);
    if (result == CURLE_OK)
        result = curl_mime_data(part, value, CURL_ZERO_TERMINATED);
    return result;
}

static CURLcode add_file(curl_mime *form, const upload_request *request)
{
    curl_mimepart *part = curl_mime_addpart(form);
    CURLcode result;
    if (!part)
        return CURLE_OUT_OF_MEMORY;
    result = curl_mime_name(part, "fileToUpload");
    if (result != CURLE_OK)
        return result;
    if (request->file_path) {
        /* Streams the file and stats it for its size, so curl can send a
         * Content-Length instead of a chunked body. */
        result = curl_mime_filedata(part, request->file_path);
    } else {
        buffer_reader *buffer = request->buffer;
        result = curl_mime_data_cb(part, (curl_off_t)buffer->size, read_buffer, seek_buffer, NULL, buffer);
    }
    /* curl_mime_filedata derives a filename with the platform's basename();
     * set ours so every OS sends the same name (and buffers get one at all). */
    if (result == CURLE_OK)
        result = curl_mime_filename(part, request->file_name);
    if (result == CURLE_OK)
        result = curl_mime_type(part, catbox_mime_type(request->file_name));
    return result;
}

static CURLcode build_form(curl_mime *form, const upload_request *request)
{
    CURLcode result = add_field(form, "reqtype", request->reqtype);
    if (result == CURLE_OK && !is_empty(request->user_hash))
        result = add_field(form, "userhash", request->user_hash);
    if (result == CURLE_OK)
        result = add_field(form, "time", request->time);
    if (result == CURLE_OK)
        result = add_field(form, "url", request->url);
    if (result == CURLE_OK && (request->file_path || request->buffer))
        result = add_file(form, request);
    return result;
}

static CURLcode configure(CURL *curl, curl_mime *form, const catbox_options *options, const char *endpoint,
                          response_body *response, char *curl_message)
{
    CURLcode result = curl_easy_setopt(curl, CURLOPT_URL, endpoint);
    if (result == CURLE_OK)
        result = curl_easy_setopt(curl, CURLOPT_MIMEPOST, form);
    if (result == CURLE_OK)
        result = curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    if (result == CURLE_OK)
        result = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_response);
    if (result == CURLE_OK)
        result = curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    if (result == CURLE_OK)
        result = curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_message);
    /* No signals, so uploads are safe to run on several threads. */
    if (result == CURLE_OK)
        result = curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (result == CURLE_OK && options && options->progress) {
        result = curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, report_progress);
        if (result == CURLE_OK)
            result = curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)options);
        if (result == CURLE_OK)
            result = curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    }
    if (result != CURLE_OK)
        return result;

    /* The endpoint can come from the command line: never let it be file:// etc.
     * Best effort, as older libcurl builds may lack the option. */
#if LIBCURL_VERSION_NUM >= 0x075500 /* 7.85.0 */
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    return CURLE_OK;
}

static char *perform(const catbox_options *options, const upload_request *request, catbox_error *error)
{
    char curl_message[CURL_ERROR_SIZE] = "";
    response_body response = {NULL, 0};
    curl_mime *form = NULL;
    char *url = NULL;
    CURLcode result;
    CURL *curl = curl_easy_init();

    if (!curl) {
        set_error(error, CATBOX_ERR_OUT_OF_MEMORY, 0, "could not create a curl handle");
        return NULL;
    }
    form = curl_mime_init(curl);
    result = form ? build_form(form, request) : CURLE_OUT_OF_MEMORY;
    if (result == CURLE_OK)
        result = configure(curl, form, options, request->endpoint, &response, curl_message);
    if (result != CURLE_OK) {
        set_error(error, result == CURLE_OUT_OF_MEMORY ? CATBOX_ERR_OUT_OF_MEMORY : CATBOX_ERR_NETWORK, 0,
                  "could not prepare the request: %s", curl_easy_strerror(result));
        goto done;
    }

    result = curl_easy_perform(curl);
    if (result == CURLE_ABORTED_BY_CALLBACK) {
        set_error(error, CATBOX_ERR_ABORTED, 0, "upload cancelled");
    } else if (result != CURLE_OK) {
        /* The error buffer has specifics such as host and port; fall back to
         * the generic text when curl left it empty. */
        set_error(error, CATBOX_ERR_NETWORK, 0, "%s", curl_message[0] ? curl_message : curl_easy_strerror(result));
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        url = catbox_parse_response(status, response.data, response.size, error);
    }

done:
    curl_mime_free(form);
    curl_easy_cleanup(curl);
    free(response.data);
    return url;
}

static const char *endpoint_or(const catbox_options *options, const char *fallback)
{
    return options && !is_empty(options->endpoint) ? options->endpoint : fallback;
}

/* Fails early, without a request, for files curl could not stream: missing,
 * directories, and things like FIFOs whose size is unknown (curl would fall
 * back to a chunked body, which catbox does not accept). */
static int check_file(const char *path, catbox_error *error)
{
    FILE *file;
#ifdef _WIN32
    struct _stat64 info; /* 64-bit sizes, unlike plain _stat */
    int failed = _stat64(path, &info) != 0;
#else
    struct stat info;
    int failed = stat(path, &info) != 0;
#endif

    if (failed) {
        if (errno == ENOENT || errno == ENOTDIR)
            set_error(error, CATBOX_ERR_FILE, 0, "no such file");
        else
            set_error(error, CATBOX_ERR_FILE, 0, "%s", strerror(errno));
        return 0;
    }
    if (IS_DIRECTORY(info.st_mode)) {
        set_error(error, CATBOX_ERR_FILE, 0, "is a directory");
        return 0;
    }
    if (!IS_REGULAR(info.st_mode)) {
        set_error(error, CATBOX_ERR_FILE, 0, "not a regular file");
        return 0;
    }
    /* Catch permission problems here instead of halfway through the request. */
    file = fopen(path, "rb");
    if (!file) {
        set_error(error, CATBOX_ERR_FILE, 0, "%s", strerror(errno));
        return 0;
    }
    fclose(file);
    return 1;
}

/* ---- public API --------------------------------------------------------- */

catbox_code catbox_global_init(void)
{
    return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK ? CATBOX_OK : CATBOX_ERR_NETWORK;
}

void catbox_global_cleanup(void)
{
    curl_global_cleanup();
}

void catbox_free(char *url)
{
    free(url);
}

static char *upload_path(const catbox_options *options, const char *path, const char *endpoint, const char *user_hash,
                         const char *time, catbox_error *error)
{
    upload_request request = {0};
    if (is_empty(path)) {
        set_error(error, CATBOX_ERR_INVALID_ARGUMENT, 0, "no file given");
        return NULL;
    }
    if (!check_file(path, error))
        return NULL;
    request.endpoint = endpoint;
    request.reqtype = "fileupload";
    request.user_hash = user_hash;
    request.time = time;
    request.file_path = path;
    request.file_name = catbox_path_basename(path);
    return perform(options, &request, error);
}

char *catbox_upload_file(const catbox_options *options, const char *path, catbox_error *error)
{
    return upload_path(options, path, endpoint_or(options, CATBOX_DEFAULT_ENDPOINT),
                       options ? options->user_hash : NULL, NULL, error);
}

char *catbox_litter_upload_file(const catbox_options *options, const char *path, catbox_expiry expiry,
                                catbox_error *error)
{
    const char *time = catbox_expiry_name(expiry);
    if (!time) {
        set_error(error, CATBOX_ERR_INVALID_ARGUMENT, 0, "expiry must be 1h, 12h, 24h or 72h");
        return NULL;
    }
    return upload_path(options, path, endpoint_or(options, CATBOX_LITTERBOX_DEFAULT_ENDPOINT), NULL, time, error);
}

char *catbox_upload_buffer(const catbox_options *options, const void *data, size_t size, const char *filename,
                           catbox_error *error)
{
    upload_request request = {0};
    buffer_reader buffer;
    if (is_empty(filename) || (data == NULL && size > 0)) {
        set_error(error, CATBOX_ERR_INVALID_ARGUMENT, 0, "a filename and data are required");
        return NULL;
    }
    buffer.data = data ? (const char *)data : "";
    buffer.size = size;
    buffer.offset = 0;
    request.endpoint = endpoint_or(options, CATBOX_DEFAULT_ENDPOINT);
    request.reqtype = "fileupload";
    request.user_hash = options ? options->user_hash : NULL;
    request.buffer = &buffer;
    request.file_name = filename;
    return perform(options, &request, error);
}

char *catbox_upload_url(const catbox_options *options, const char *url, catbox_error *error)
{
    upload_request request = {0};
    if (is_empty(url)) {
        set_error(error, CATBOX_ERR_INVALID_ARGUMENT, 0, "no URL given");
        return NULL;
    }
    request.endpoint = endpoint_or(options, CATBOX_DEFAULT_ENDPOINT);
    request.reqtype = "urlupload";
    request.user_hash = options ? options->user_hash : NULL;
    request.url = url;
    return perform(options, &request, error);
}
