/*
 * progress.c - stderr progress bar. Redraws with '\r' and pads over the
 * previous line with spaces instead of ANSI erase codes, which older Windows
 * consoles don't understand.
 */
#include "progress.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  include <io.h>
#else
#  include <unistd.h>
#endif

#define BAR_WIDTH 24

int stderr_is_terminal(void)
{
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

void progress_bar_init(progress_bar *bar, const char *label)
{
    bar->label = label;
    bar->last_permille = -1;
    bar->line_width = 0;
}

void format_bytes(char *out, size_t size, int64_t bytes)
{
    static const char *const units[] = {"B", "KB", "MB", "GB"};
    double value = (double)bytes;
    size_t unit = 0;
    while (value >= 1024 && unit + 1 < sizeof units / sizeof units[0]) {
        value /= 1024;
        unit++;
    }
    if (unit == 0)
        snprintf(out, size, "%lld B", (long long)bytes);
    else
        snprintf(out, size, "%.1f %s", value, units[unit]);
}

/* Terminal columns taken by UTF-8 text: one per code point, which is right for
 * most file names (wide CJK characters are the exception). */
static size_t display_width(const char *text)
{
    size_t width = 0;
    for (; *text; text++) {
        if (((unsigned char)*text & 0xC0) != 0x80) /* skip continuation bytes */
            width++;
    }
    return width;
}

int progress_bar_update(void *user_data, int64_t sent, int64_t total)
{
    progress_bar *bar = user_data;
    char meter[BAR_WIDTH + 1];
    char sent_text[32], total_text[32], status[128];
    size_t width;
    int permille, filled;

    if (total <= 0)
        return 0;
    permille = (int)((sent < total ? sent : total) * 1000 / total);
    if (permille == bar->last_permille)
        return 0;
    bar->last_permille = permille;

    filled = permille * BAR_WIDTH / 1000;
    memset(meter, ' ', BAR_WIDTH);
    memset(meter, '=', (size_t)filled);
    if (filled < BAR_WIDTH)
        meter[filled] = '>';
    meter[BAR_WIDTH] = '\0';

    format_bytes(sent_text, sizeof sent_text, sent);
    format_bytes(total_text, sizeof total_text, total);
    snprintf(status, sizeof status, " [%s] %5.1f%% %s/%s", meter, permille / 10.0, sent_text, total_text);

    width = display_width(bar->label) + strlen(status);
    fprintf(stderr, "\r%s%s", bar->label, status);
    if (bar->line_width > width)
        fprintf(stderr, "%*s", (int)(bar->line_width - width), "");
    fflush(stderr);
    bar->line_width = width;
    return 0;
}

void progress_bar_finish(progress_bar *bar)
{
    if (bar->line_width > 0) {
        fprintf(stderr, "\r%*s\r", (int)bar->line_width, "");
        fflush(stderr);
        bar->line_width = 0;
    }
}
