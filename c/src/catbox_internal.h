/*
 * Library internals shared with the unit tests. Not installed and not
 * exported from a shared build.
 */
#ifndef CATBOX_INTERNAL_H
#define CATBOX_INTERNAL_H

#include "catbox.h"

/* Turns an HTTP status and response body into a URL (allocated, release with
 * catbox_free) or NULL plus a filled-in error. `body` need not be
 * NUL-terminated and may be NULL when `size` is 0. */
char *catbox_parse_response(long status, const char *body, size_t size, catbox_error *error);

#endif /* CATBOX_INTERNAL_H */
