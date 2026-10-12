/*
 * TTY detection and a one-line progress bar on stderr, so stdout stays clean
 * for piping URLs.
 */
#ifndef CATBOX_PROGRESS_H
#define CATBOX_PROGRESS_H

#include <stddef.h>
#include <stdint.h>

/* Non-zero when stderr is a terminal (isatty, or _isatty on Windows). */
int stderr_is_terminal(void);

typedef struct progress_bar {
    const char *label;
    int last_permille; /* -1 before the first draw */
    size_t line_width; /* columns drawn last time, to pad over */
} progress_bar;

void progress_bar_init(progress_bar *bar, const char *label);

/* A catbox_progress_fn: pass the bar as the callback's user data. Redraws at
 * most once per 0.1% so fast uploads don't flood the terminal. Returns 0. */
int progress_bar_update(void *bar, int64_t sent, int64_t total);

/* Clears the line, if anything was drawn. */
void progress_bar_finish(progress_bar *bar);

/* "512 B", "1.5 KB", "24.0 MB", ... into `out`. */
void format_bytes(char *out, size_t size, int64_t bytes);

#endif /* CATBOX_PROGRESS_H */
