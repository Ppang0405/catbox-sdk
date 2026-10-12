/*
 * cli.c - hand-written argument parsing (MSVC has no getopt) and help text.
 *
 * Accepted forms: "--name value", "--name=value", "-t value", "-tvalue",
 * options before or after the operands, and "--" to end option parsing.
 */
#include "cli.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

enum option_id { OPT_HELP, OPT_QUIET, OPT_USER_HASH, OPT_ENDPOINT, OPT_TIME };

#define FOR(command) (1u << (command))
#define ALL_COMMANDS (FOR(CLI_UPLOAD) | FOR(CLI_UPLOAD_URL) | FOR(CLI_LITTER))

static const struct option_spec {
    enum option_id id;
    char short_name; /* 0 if none */
    const char *long_name;
    int takes_value;
    unsigned commands; /* bit set of cli_command values that accept it */
} option_specs[] = {
    {OPT_HELP, 'h', "help", 0, ALL_COMMANDS},
    /* upload-url accepts -q too, as the other implementations do; it has no
     * progress to hide, so its help doesn't list it. */
    {OPT_QUIET, 'q', "quiet", 0, ALL_COMMANDS},
    {OPT_USER_HASH, 0, "user-hash", 1, FOR(CLI_UPLOAD) | FOR(CLI_UPLOAD_URL)},
    {OPT_ENDPOINT, 0, "endpoint", 1, ALL_COMMANDS}, /* hidden: for tests */
    {OPT_TIME, 't', "time", 1, FOR(CLI_LITTER)},
};

static const struct {
    cli_command command;
    const char *name;
} command_names[] = {
    {CLI_UPLOAD, "upload"},
    {CLI_UPLOAD_URL, "upload-url"},
    {CLI_LITTER, "litter"},
};

const char *cli_command_name(cli_command command)
{
    size_t i;
    for (i = 0; i < sizeof command_names / sizeof command_names[0]; i++) {
        if (command_names[i].command == command)
            return command_names[i].name;
    }
    return "";
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
static int usage_error(cli_args *args, const char *format, ...)
{
    va_list values;
    args->action = CLI_USAGE_ERROR;
    va_start(values, format);
    vsnprintf(args->error, sizeof args->error, format, values);
    va_end(values);
    return 0;
}

/* Finds the option `arg` names, splitting off an attached value ("--time=1h"
 * or "-t1h") into *attached. Returns NULL if no option matches. */
static const struct option_spec *find_option(const char *arg, const char **attached)
{
    size_t i;
    *attached = NULL;
    if (arg[1] == '-') {
        const char *name = arg + 2;
        const char *equals = strchr(name, '=');
        size_t length = equals ? (size_t)(equals - name) : strlen(name);
        for (i = 0; i < sizeof option_specs / sizeof option_specs[0]; i++) {
            if (strlen(option_specs[i].long_name) == length && strncmp(option_specs[i].long_name, name, length) == 0) {
                *attached = equals ? equals + 1 : NULL;
                return &option_specs[i];
            }
        }
        return NULL;
    }
    for (i = 0; i < sizeof option_specs / sizeof option_specs[0]; i++) {
        if (option_specs[i].short_name != 0 && option_specs[i].short_name == arg[1]) {
            *attached = arg[2] != '\0' ? arg + 2 : NULL;
            return &option_specs[i];
        }
    }
    return NULL;
}

/* Applies one option. *index points at it and is advanced past a separate
 * value. Returns 0 after recording a usage error. */
static int apply_option(cli_args *args, int argc, char **argv, int *index)
{
    const char *arg = argv[*index];
    const char *value = NULL;
    const struct option_spec *spec = find_option(arg, &value);

    if (!spec || !(spec->commands & FOR(args->command)))
        return usage_error(args, "unknown option '%s'", arg);
    if (!spec->takes_value && value) {
        /* "--quiet=1" is not a flag, and bundles like "-qt" are not supported. */
        if (arg[1] == '-')
            return usage_error(args, "option '--%s' takes no value", spec->long_name);
        return usage_error(args, "unknown option '%s'", arg);
    }
    if (spec->takes_value && !value) {
        if (*index + 1 >= argc)
            return usage_error(args, "option '%s' needs a value", arg);
        value = argv[++*index];
    }

    switch (spec->id) {
    case OPT_HELP:
        args->action = CLI_SHOW_HELP;
        break;
    case OPT_QUIET:
        args->quiet = 1;
        break;
    case OPT_USER_HASH:
        args->user_hash = value;
        break;
    case OPT_ENDPOINT:
        args->endpoint = value;
        break;
    case OPT_TIME:
        if (!catbox_expiry_parse(value, &args->expiry))
            return usage_error(args, "invalid time '%s' (choose 1h, 12h, 24h or 72h)", value);
        break;
    }
    return 1;
}

static void parse_command(int argc, char **argv, cli_args *args)
{
    int options_done = 0;
    int i;

    for (i = 2; i < argc && args->action == CLI_RUN; i++) {
        const char *arg = argv[i];
        if (options_done || arg[0] != '-' || arg[1] == '\0') { /* "-" is an operand */
            args->inputs[args->input_count++] = arg;
        } else if (strcmp(arg, "--") == 0) {
            options_done = 1;
        } else if (!apply_option(args, argc, argv, &i)) {
            return;
        }
    }
    if (args->action == CLI_RUN && args->input_count == 0)
        usage_error(args, "%s", args->command == CLI_UPLOAD_URL ? "no URLs given" : "no files given");
}

void cli_parse(int argc, char **argv, cli_args *args)
{
    const char *first;
    size_t i;

    memset(args, 0, sizeof *args);
    args->action = CLI_RUN;
    args->expiry = CATBOX_EXPIRY_72H;

    if (argc < 2) {
        usage_error(args, "no command given");
        return;
    }
    first = argv[1];
    if (strcmp(first, "--help") == 0 || strcmp(first, "-h") == 0) {
        args->action = CLI_SHOW_HELP;
        return;
    }
    if (strcmp(first, "--version") == 0) {
        args->action = CLI_SHOW_VERSION;
        return;
    }
    for (i = 0; i < sizeof command_names / sizeof command_names[0]; i++) {
        if (strcmp(first, command_names[i].name) == 0)
            args->command = command_names[i].command;
    }
    if (args->command == CLI_NO_COMMAND) {
        usage_error(args, first[0] == '-' ? "unknown option '%s'" : "unknown command '%s'", first);
        return;
    }

    /* Room for every remaining argument to be an operand. */
    args->inputs = malloc((size_t)argc * sizeof *args->inputs);
    if (!args->inputs) {
        usage_error(args, "out of memory");
        return;
    }
    parse_command(argc, argv, args);
}

void cli_args_free(cli_args *args)
{
    free(args->inputs);
    args->inputs = NULL;
    args->input_count = 0;
}

void cli_print_help(FILE *out, cli_command command)
{
    switch (command) {
    case CLI_NO_COMMAND:
        fputs("Upload files to catbox.moe and litterbox.catbox.moe.\n"
              "\n"
              "Usage: catbox <command> [options] ARG...\n"
              "\n"
              "Commands:\n"
              "  upload       Upload files to catbox.moe permanently (max 200 MB each)\n"
              "  upload-url   Have catbox.moe fetch and store files from remote URLs\n"
              "  litter       Upload files to litterbox.catbox.moe temporarily (max 1 GB each)\n"
              "\n"
              "Options:\n"
              "  -h, --help   Show this help\n"
              "  --version    Show the version\n"
              "\n"
              "Run 'catbox <command> --help' for the options of a command.\n",
              out);
        break;
    case CLI_UPLOAD:
        fputs("Upload files to catbox.moe permanently (max 200 MB each).\n"
              "\n"
              "Usage: catbox upload [--user-hash HASH] [-q | --quiet] FILE...\n"
              "\n"
              "Options:\n"
              "  --user-hash HASH  Your catbox user hash, so uploads show up in your account\n"
              "                    (default: $CATBOX_USERHASH)\n"
              "  -q, --quiet       Don't show upload progress\n"
              "  -h, --help        Show this help\n",
              out);
        break;
    case CLI_UPLOAD_URL:
        fputs("Have catbox.moe fetch and store files from remote URLs.\n"
              "\n"
              "Usage: catbox upload-url [--user-hash HASH] URL...\n"
              "\n"
              "Options:\n"
              "  --user-hash HASH  Your catbox user hash, so uploads show up in your account\n"
              "                    (default: $CATBOX_USERHASH)\n"
              "  -h, --help        Show this help\n",
              out);
        break;
    case CLI_LITTER:
        fputs("Upload files to litterbox.catbox.moe temporarily (max 1 GB each).\n"
              "\n"
              "Usage: catbox litter [-t | --time 1h|12h|24h|72h] [-q | --quiet] FILE...\n"
              "\n"
              "Options:\n"
              "  -t, --time TIME   How long to keep the files: 1h, 12h, 24h or 72h (default: 72h)\n"
              "  -q, --quiet       Don't show upload progress\n"
              "  -h, --help        Show this help\n",
              out);
        break;
    }
}
