/*
 * main.c - the catbox command. See conformance/SPEC.md for the behavior
 * shared by every implementation in this repository.
 */
#include <stdio.h>
#include <stdlib.h>

#include "catbox.h"
#include "cli.h"
#include "progress.h"

/* "catbox: <item>: <reason>" on one line, even if the server's text had
 * line breaks in it. */
static void report_failure(const char *item, const char *reason)
{
    const char *c;
    fprintf(stderr, "catbox: %s: ", item);
    for (c = reason; *c; c++)
        fputc(*c == '\r' || *c == '\n' || *c == '\t' ? ' ' : *c, stderr);
    fputc('\n', stderr);
}

static char *upload_one(const cli_args *args, catbox_options *options, const char *input, catbox_error *error)
{
    switch (args->command) {
    case CLI_UPLOAD:
        return catbox_upload_file(options, input, error);
    case CLI_LITTER:
        return catbox_litter_upload_file(options, input, args->expiry, error);
    case CLI_UPLOAD_URL:
        return catbox_upload_url(options, input, error);
    case CLI_NO_COMMAND:
        break;
    }
    return NULL;
}

/* Uploads every input in order, printing one URL per line on stdout and
 * continuing past failures. Returns 1 if all of them succeeded. */
static int run(const cli_args *args)
{
    catbox_options options = {0};
    int show_progress = !args->quiet && args->command != CLI_UPLOAD_URL && stderr_is_terminal();
    int all_ok = 1;
    int i;

    options.endpoint = args->endpoint;
    if (args->command != CLI_LITTER) /* litterbox has no accounts */
        options.user_hash = args->user_hash ? args->user_hash : getenv("CATBOX_USERHASH");

    for (i = 0; i < args->input_count; i++) {
        const char *input = args->inputs[i];
        catbox_error error = {CATBOX_OK, 0, ""};
        progress_bar bar;
        char *url;

        progress_bar_init(&bar, catbox_path_basename(input));
        if (show_progress) {
            options.progress = progress_bar_update;
            options.progress_data = &bar;
        }
        url = upload_one(args, &options, input, &error);
        if (show_progress)
            progress_bar_finish(&bar);

        if (url) {
            printf("%s\n", url);
            fflush(stdout); /* so a pipe sees each URL as soon as it exists */
            catbox_free(url);
        } else {
            all_ok = 0;
            report_failure(input, error.message);
        }
    }
    return all_ok;
}

int main(int argc, char **argv)
{
    cli_args args;
    int status = 0;

    cli_parse(argc, argv, &args);
    switch (args.action) {
    case CLI_SHOW_HELP:
        cli_print_help(stdout, args.command);
        break;
    case CLI_SHOW_VERSION:
        printf("catbox %s\n", CATBOX_VERSION);
        break;
    case CLI_USAGE_ERROR:
        fprintf(stderr, "catbox: %s\n", args.error);
        if (args.command == CLI_NO_COMMAND)
            fputs("Try 'catbox --help' for more information.\n", stderr);
        else
            fprintf(stderr, "Try 'catbox %s --help' for more information.\n", cli_command_name(args.command));
        status = 2;
        break;
    case CLI_RUN:
        if (catbox_global_init() != CATBOX_OK) {
            fputs("catbox: could not initialize libcurl\n", stderr);
            status = 1;
            break;
        }
        status = run(&args) ? 0 : 1;
        catbox_global_cleanup();
        break;
    }
    cli_args_free(&args);
    return status;
}
