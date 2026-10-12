/*
 * catbox.h - upload files to catbox.moe and litterbox.catbox.moe.
 *
 * A small C11 library on top of libcurl. Every upload is one blocking
 * multipart/form-data POST; file uploads are streamed from disk.
 *
 * Usage:
 *
 *     catbox_global_init();
 *     catbox_error err;
 *     char *url = catbox_upload_file(NULL, "photo.jpg", &err);
 *     if (url) {
 *         puts(url);
 *         catbox_free(url);
 *     } else {
 *         fprintf(stderr, "upload failed: %s\n", err.message);
 *     }
 *     catbox_global_cleanup();
 *
 * Memory and ownership:
 *
 *   - Every upload function returns a newly allocated, NUL-terminated URL on
 *     success, or NULL on failure. Release the URL with catbox_free(), not
 *     free(): on Windows the library may use a different C runtime heap.
 *   - catbox_error is a plain struct owned by the caller (usually on the
 *     stack). Its message lives inside the struct, so nothing needs freeing.
 *     Passing NULL for the error is allowed when the caller does not care.
 *   - Strings passed in (paths, URLs, options) are only read during the call
 *     and are never retained.
 *
 * Threads: call catbox_global_init() once before any other thread starts and
 * catbox_global_cleanup() once at the end. In between, uploads may run
 * concurrently on different threads; each call uses its own curl handle.
 */
#ifndef CATBOX_H
#define CATBOX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Symbol visibility. Static builds (the default) need nothing; a shared build
 * defines CATBOX_SHARED for consumers and CATBOX_BUILDING inside the library.
 * The CMake targets set both automatically. */
#if defined(CATBOX_SHARED) && defined(_WIN32)
#  ifdef CATBOX_BUILDING
#    define CATBOX_API __declspec(dllexport)
#  else
#    define CATBOX_API __declspec(dllimport)
#  endif
#elif defined(CATBOX_SHARED) && defined(__GNUC__)
#  define CATBOX_API __attribute__((visibility("default")))
#else
#  define CATBOX_API
#endif

#define CATBOX_VERSION "0.1.0"

#define CATBOX_DEFAULT_ENDPOINT "https://catbox.moe/user/api.php"
#define CATBOX_LITTERBOX_DEFAULT_ENDPOINT "https://litterbox.catbox.moe/resources/internals/api.php"

/* What went wrong. */
typedef enum catbox_code {
    CATBOX_OK = 0,
    CATBOX_ERR_INVALID_ARGUMENT, /* NULL or empty input, unknown expiry */
    CATBOX_ERR_FILE,             /* missing, a directory, or unreadable */
    CATBOX_ERR_NETWORK,          /* curl could not complete the request */
    CATBOX_ERR_HTTP_STATUS,      /* the server answered with a non-2xx status */
    CATBOX_ERR_SERVER,           /* 2xx, but the body was not a URL */
    CATBOX_ERR_ABORTED,          /* the progress callback asked to stop */
    CATBOX_ERR_OUT_OF_MEMORY
} catbox_code;

#define CATBOX_ERROR_MESSAGE_SIZE 512

/* Filled in by every upload function. On success code is CATBOX_OK, the
 * message is empty and http_status holds the response status. */
typedef struct catbox_error {
    catbox_code code;
    /* HTTP status of the response, or 0 if none was received. */
    long http_status;
    /* One human-readable sentence, e.g. "server error: No files given.".
     * Catbox reports most failures as plain text with status 200, so the
     * server's body text is included (possibly truncated). */
    char message[CATBOX_ERROR_MESSAGE_SIZE];
} catbox_error;

/* Called while the request body is sent with the bytes sent so far and the
 * total body size (the whole multipart form, so slightly more than the file).
 * Return 0 to continue, or non-zero to cancel with CATBOX_ERR_ABORTED.
 * Runs on the thread that called the upload function. */
typedef int (*catbox_progress_fn)(void *user_data, int64_t sent, int64_t total);

/* Per-call settings. Pass NULL, or a zero-initialized struct, for defaults. */
typedef struct catbox_options {
    /* API URL; NULL means the service's default endpoint. */
    const char *endpoint;
    /* Catbox account hash so uploads show up in the account. NULL or "" means
     * anonymous. Litterbox ignores it. */
    const char *user_hash;
    /* Optional upload progress callback and the pointer passed to it. */
    catbox_progress_fn progress;
    void *progress_data;
} catbox_options;

/* How long litterbox keeps a file. */
typedef enum catbox_expiry {
    CATBOX_EXPIRY_1H,
    CATBOX_EXPIRY_12H,
    CATBOX_EXPIRY_24H,
    CATBOX_EXPIRY_72H
} catbox_expiry;

/* Initialize and release libcurl's global state (wraps curl_global_init and
 * curl_global_cleanup). Returns CATBOX_OK or CATBOX_ERR_NETWORK. */
CATBOX_API catbox_code catbox_global_init(void);
CATBOX_API void catbox_global_cleanup(void);

/* Uploads the file at `path` to catbox.moe permanently (200 MB max). The file
 * is streamed from disk and sent under its base name. */
CATBOX_API char *catbox_upload_file(const catbox_options *options, const char *path, catbox_error *error);

/* Uploads `size` bytes from memory, sent under `filename` (whose extension
 * picks the Content-Type). The buffer is only read during the call. */
CATBOX_API char *catbox_upload_buffer(const catbox_options *options, const void *data, size_t size,
                                      const char *filename, catbox_error *error);

/* Asks catbox.moe to fetch `url` and store a copy. Progress is not reported. */
CATBOX_API char *catbox_upload_url(const catbox_options *options, const char *url, catbox_error *error);

/* Uploads the file at `path` to litterbox.catbox.moe, kept for `expiry`
 * (1 GB max). options->user_hash is ignored. */
CATBOX_API char *catbox_litter_upload_file(const catbox_options *options, const char *path, catbox_expiry expiry,
                                           catbox_error *error);

/* Frees a URL returned by an upload function. NULL is allowed. */
CATBOX_API void catbox_free(char *url);

/* "1h", "12h", "24h" or "72h"; NULL for a value outside the enum. */
CATBOX_API const char *catbox_expiry_name(catbox_expiry expiry);

/* Parses "1h", "12h", "24h" or "72h". Returns 1 and sets *expiry on success,
 * 0 for anything else. */
CATBOX_API int catbox_expiry_parse(const char *name, catbox_expiry *expiry);

/* The Content-Type sent for `filename`, chosen from a fixed extension table
 * (case-insensitive), or "application/octet-stream". Never NULL; the string
 * is static. */
CATBOX_API const char *catbox_mime_type(const char *filename);

/* The final component of `path`, i.e. the filename catbox_upload_file sends.
 * Splits on '/', and also on '\\' and a drive letter's ':' on Windows.
 * Returns a pointer into `path`. */
CATBOX_API const char *catbox_path_basename(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CATBOX_H */
