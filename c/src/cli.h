/*
 * Command-line parsing for the catbox tool. Pure (no I/O besides printing
 * help), so the unit tests can drive it with made-up argv arrays.
 */
#ifndef CATBOX_CLI_H
#define CATBOX_CLI_H

#include <stdio.h>

#include "catbox.h"

typedef enum cli_command {
    CLI_NO_COMMAND,
    CLI_UPLOAD,
    CLI_UPLOAD_URL,
    CLI_LITTER
} cli_command;

typedef enum cli_action {
    CLI_RUN,          /* upload args.inputs with args.command */
    CLI_SHOW_HELP,    /* print help for args.command (or the overview) */
    CLI_SHOW_VERSION,
    CLI_USAGE_ERROR   /* args.error says what is wrong */
} cli_action;

typedef struct cli_args {
    cli_action action;
    cli_command command;
    const char *user_hash; /* NULL unless --user-hash was given */
    const char *endpoint;  /* NULL unless --endpoint was given */
    int quiet;
    catbox_expiry expiry;  /* litter only; defaults to 72h */
    const char **inputs;   /* FILE or URL arguments in order, pointing into argv */
    int input_count;
    char error[256];
} cli_args;

/* Parses argv into `args`. Always fills in args->action; release with
 * cli_args_free whatever the outcome. */
void cli_parse(int argc, char **argv, cli_args *args);
void cli_args_free(cli_args *args);

/* Help for one command, or the overview for CLI_NO_COMMAND. */
void cli_print_help(FILE *out, cli_command command);

/* "upload", "upload-url", "litter", or "" for CLI_NO_COMMAND. */
const char *cli_command_name(cli_command command);

#endif /* CATBOX_CLI_H */
