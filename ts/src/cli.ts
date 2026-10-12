#!/usr/bin/env bun
/** The `catbox` command. See conformance/SPEC.md for the behavior shared by every implementation. */

import { stat } from "node:fs/promises";
import { basename } from "node:path";
import { parseArgs, type ParseArgsOptionsConfig } from "node:util";

import { CatboxClient } from "./catbox.ts";
import { EXPIRY_TIMES, isExpiry, LitterboxClient } from "./litterbox.ts";
import type { ProgressCallback } from "./multipart.ts";
import { ProgressBar } from "./progress.ts";
import { VERSION } from "./version.ts";

/** Where the command reads its environment and writes its output; swapped out in tests. */
export interface Io {
  stdout(text: string): void;
  stderr(text: string): void;
  stderrIsTTY: boolean;
  env: Record<string, string | undefined>;
}

export const EXIT_OK = 0;
export const EXIT_FAILURE = 1;
export const EXIT_USAGE = 2;

const HELP = `Usage: catbox <command> [options] <arguments>...

Upload files to catbox.moe and litterbox.catbox.moe.

Commands:
  upload       Upload files to catbox.moe permanently (max 200 MB each)
  upload-url   Have catbox.moe fetch and store files from remote URLs
  litter       Upload files to litterbox.catbox.moe temporarily (max 1 GB each)

Options:
  -h, --help     Show this help
  -V, --version  Show the version

Run 'catbox <command> --help' for the options of a command.
`;

const USER_HASH_HELP = `      --user-hash HASH  Your catbox user hash, so uploads show up in your account
                        (default: $CATBOX_USERHASH)`;

const COMMAND_HELP = {
  upload: `Usage: catbox upload [options] FILE...

Upload files to catbox.moe permanently (max 200 MB each).

Options:
${USER_HASH_HELP}
  -q, --quiet           Don't show upload progress
  -h, --help            Show this help
`,
  "upload-url": `Usage: catbox upload-url [options] URL...

Have catbox.moe fetch and store files from remote URLs.

Options:
${USER_HASH_HELP}
  -h, --help            Show this help
`,
  litter: `Usage: catbox litter [options] FILE...

Upload files to litterbox.catbox.moe temporarily (max 1 GB each).

Options:
  -t, --time TIME  How long to keep the files: ${EXPIRY_TIMES.join(", ")} (default: 72h)
  -q, --quiet      Don't show upload progress
  -h, --help       Show this help
`,
};

type CommandName = keyof typeof COMMAND_HELP;

// Every command takes these. --endpoint exists for testing and is left out of the help.
const COMMON_OPTIONS = {
  quiet: { type: "boolean", short: "q" },
  endpoint: { type: "string" },
  help: { type: "boolean", short: "h" },
} as const satisfies ParseArgsOptionsConfig;

const USER_HASH_OPTION = {
  "user-hash": { type: "string" },
} as const satisfies ParseArgsOptionsConfig;

/** Bad command-line input: reported with a pointer to --help, before any request is sent. */
class UsageError extends Error {
  readonly command: CommandName | undefined;

  constructor(message: string, command?: CommandName) {
    super(message);
    this.command = command;
  }
}

/** Runs the command with `argv` (without the program name) and returns the exit status. */
export async function main(argv: string[], io: Io = processIo()): Promise<number> {
  try {
    return await dispatch(argv, io);
  } catch (error) {
    if (!(error instanceof UsageError)) throw error;
    const help = error.command ? `catbox ${error.command} --help` : "catbox --help";
    io.stderr(`catbox: ${error.message}\nTry '${help}' for more information.\n`);
    return EXIT_USAGE;
  }
}

async function dispatch(argv: string[], io: Io): Promise<number> {
  const [command, ...args] = argv;
  switch (command) {
    case undefined:
      throw new UsageError("missing command (upload, upload-url or litter)");
    case "-h":
    case "--help":
    case "help":
      io.stdout(HELP);
      return EXIT_OK;
    case "-V":
    case "--version":
      io.stdout(`catbox ${VERSION}\n`);
      return EXIT_OK;
    case "upload":
      return upload(args, io);
    case "upload-url":
      return uploadUrl(args, io);
    case "litter":
      return litter(args, io);
    default:
      throw new UsageError(`unknown command '${command}'`);
  }
}

async function upload(args: string[], io: Io): Promise<number> {
  const { values, positionals } = parse("upload", args, { ...COMMON_OPTIONS, ...USER_HASH_OPTION });
  if (values.help) return showHelp("upload", io);
  requireArguments("upload", positionals, "no files given");
  const client = new CatboxClient({
    userHash: values["user-hash"] ?? io.env.CATBOX_USERHASH,
    endpoint: endpoint("upload", values.endpoint),
  });
  return uploadEach(positionals, values.quiet ?? false, io, (path, progress) => client.uploadFile(path, { progress }));
}

async function uploadUrl(args: string[], io: Io): Promise<number> {
  const { values, positionals } = parse("upload-url", args, { ...COMMON_OPTIONS, ...USER_HASH_OPTION });
  if (values.help) return showHelp("upload-url", io);
  requireArguments("upload-url", positionals, "no URLs given");
  const client = new CatboxClient({
    userHash: values["user-hash"] ?? io.env.CATBOX_USERHASH,
    endpoint: endpoint("upload-url", values.endpoint),
  });
  let failed = false;
  for (const url of positionals) {
    try {
      io.stdout(`${await client.uploadUrl(url)}\n`);
    } catch (error) {
      failed = true;
      reportFailure(io, url, error);
    }
  }
  return failed ? EXIT_FAILURE : EXIT_OK;
}

async function litter(args: string[], io: Io): Promise<number> {
  const { values, positionals } = parse("litter", args, {
    ...COMMON_OPTIONS,
    time: { type: "string", short: "t", default: "72h" },
  });
  if (values.help) return showHelp("litter", io);
  const expiry = values.time;
  if (!isExpiry(expiry)) {
    throw new UsageError(`invalid --time '${expiry}' (choose from ${EXPIRY_TIMES.join(", ")})`, "litter");
  }
  requireArguments("litter", positionals, "no files given");
  const client = new LitterboxClient({ endpoint: endpoint("litter", values.endpoint) });
  return uploadEach(positionals, values.quiet ?? false, io, (path, progress) =>
    client.uploadFile(path, { expiry, progress }),
  );
}

/**
 * Uploads each file in turn, printing one URL per line on stdout. Keeps going past
 * failures and returns a failure status at the end if any upload failed.
 */
async function uploadEach(
  paths: string[],
  quiet: boolean,
  io: Io,
  uploadFile: (path: string, progress: ProgressCallback | undefined) => Promise<string>,
): Promise<number> {
  let failed = false;
  for (const path of paths) {
    const problem = await checkFile(path);
    if (problem) {
      failed = true;
      io.stderr(`catbox: ${path}: ${problem}\n`);
      continue;
    }

    const bar = quiet || !io.stderrIsTTY ? undefined : new ProgressBar(basename(path), io.stderr);
    try {
      const url = await uploadFile(path, bar && ((sent, total) => bar.update(sent, total)));
      bar?.finish();
      io.stdout(`${url}\n`);
    } catch (error) {
      bar?.finish();
      failed = true;
      reportFailure(io, path, error);
    }
  }
  return failed ? EXIT_FAILURE : EXIT_OK;
}

/** Says why `path` can't be uploaded, so no request is sent for it. */
async function checkFile(path: string): Promise<string | undefined> {
  try {
    const info = await stat(path);
    if (info.isDirectory()) return "is a directory";
    if (!info.isFile()) return "not a regular file";
    return undefined;
  } catch (error) {
    const code = (error as NodeJS.ErrnoException).code;
    return code === "ENOENT" || code === "ENOTDIR" ? "no such file" : describe(error);
  }
}

function parse<const Options extends ParseArgsOptionsConfig>(command: CommandName, args: string[], options: Options) {
  try {
    return parseArgs({ args, options, allowPositionals: true, strict: true });
  } catch (error) {
    // Node's message for unknown options goes on to explain `--`; the first sentence is enough.
    const message = describe(error).replace(/\. To specify a positional argument.*$/s, "");
    throw new UsageError(message.charAt(0).toLowerCase() + message.slice(1), command);
  }
}

function requireArguments(command: CommandName, positionals: string[], message: string): void {
  if (positionals.length === 0) throw new UsageError(message, command);
}

function endpoint(command: CommandName, value: string | undefined): string | undefined {
  if (value !== undefined && !URL.canParse(value)) {
    throw new UsageError(`invalid --endpoint URL '${value}'`, command);
  }
  return value;
}

function showHelp(command: CommandName, io: Io): number {
  io.stdout(COMMAND_HELP[command]);
  return EXIT_OK;
}

function reportFailure(io: Io, argument: string, error: unknown): void {
  io.stderr(`catbox: ${argument}: ${describe(error)}\n`);
}

function describe(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function processIo(): Io {
  return {
    stdout: (text) => process.stdout.write(text),
    stderr: (text) => process.stderr.write(text),
    stderrIsTTY: process.stderr.isTTY === true,
    env: process.env,
  };
}

if (import.meta.main) {
  process.exitCode = await main(process.argv.slice(2));
}
