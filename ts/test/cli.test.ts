import { afterAll, beforeEach, describe, expect, test } from "bun:test";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { type Io, main } from "../src/cli.ts";
import { formatBytes, ProgressBar } from "../src/progress.ts";
import { startServer } from "./server.ts";

const server = startServer();
const dir = await mkdtemp(join(tmpdir(), "catbox-cli-test-"));

afterAll(async () => {
  server.stop();
  await rm(dir, { recursive: true, force: true });
});

beforeEach(() => {
  server.requests.length = 0;
});

async function run(args: string[], options: { env?: Record<string, string>; tty?: boolean } = {}) {
  let stdout = "";
  let stderr = "";
  const io: Io = {
    stdout: (text) => void (stdout += text),
    stderr: (text) => void (stderr += text),
    stderrIsTTY: options.tty ?? false,
    env: options.env ?? {},
  };
  const code = await main(args, io);
  return { code, stdout, stderr };
}

async function file(name: string, content = "content"): Promise<string> {
  const path = join(dir, name);
  await writeFile(path, content);
  return path;
}

describe("usage", () => {
  test("--version and --help exit 0 on stdout", async () => {
    expect(await run(["--version"])).toEqual({ code: 0, stdout: "catbox 0.1.0\n", stderr: "" });
    const help = await run(["--help"]);
    expect(help.code).toBe(0);
    for (const command of ["upload", "upload-url", "litter"]) expect(help.stdout).toContain(command);
    expect(help.stdout).not.toContain("--endpoint");
    expect((await run(["litter", "--help"])).stdout).toContain("--time");
  });

  test.each([
    [[], "missing command"],
    [["frobnicate"], "unknown command 'frobnicate'"],
    [["upload"], "no files given"],
    [["upload-url"], "no URLs given"],
    [["upload", "--no-such-option", "a.txt"], "unknown option '--no-such-option'"],
    [["litter", "--user-hash", "H", "a.txt"], "unknown option '--user-hash'"],
    [["litter", "-t", "5h", "a.txt"], "invalid --time '5h'"],
    [["litter", "-t"], "argument missing"],
    [["upload", "--endpoint", "nope", "a.txt"], "invalid --endpoint URL"],
  ])("%j is a usage error", async (args, message) => {
    const result = await run(args);
    expect(result.code).toBe(2);
    expect(result.stdout).toBe("");
    expect(result.stderr).toContain(message);
    expect(server.requests).toHaveLength(0);
  });
});

describe("upload", () => {
  test("prints URLs in order and reports failures on stderr", async () => {
    const first = await file("one.txt");
    const second = await file("two.png");
    const result = await run(["upload", "--endpoint", server.url, first, join(dir, "missing.gif"), dir, second]);
    expect(result.code).toBe(1);
    expect(result.stdout).toMatch(/^https:\/\/\S+\.txt\nhttps:\/\/\S+\.png\n$/);
    expect(result.stderr).toBe(`catbox: ${join(dir, "missing.gif")}: no such file\ncatbox: ${dir}: is a directory\n`);
    expect(server.requests).toHaveLength(2);
  });

  test("reports server errors", async () => {
    const result = await run(["upload", "--endpoint", server.url, await file("badresp.txt"), await file("http500.txt")]);
    expect(result.code).toBe(1);
    expect(result.stdout).toBe("");
    expect(result.stderr).toContain("badresp.txt: server error: No files given.\n");
    expect(result.stderr).toContain("http500.txt: server returned HTTP 500: Internal Server Error\n");
  });

  test("takes the user hash from the flag, then CATBOX_USERHASH", async () => {
    const path = await file("a.txt");
    const env = { CATBOX_USERHASH: "ENV" };
    await run(["upload", "--endpoint", server.url, path], { env });
    await run(["upload", "--endpoint", server.url, "--user-hash", "FLAG", path], { env });
    await run(["upload", "--endpoint", server.url, path]);
    expect(server.requests.map((request) => request.fields.userhash)).toEqual(["ENV", "FLAG", undefined]);
  });

  test("draws progress only on a terminal and not with --quiet", async () => {
    const path = await file("big.bin", "x".repeat(100_000));
    const drawn = await run(["upload", "--endpoint", server.url, path], { tty: true });
    expect(drawn.code).toBe(0);
    expect(drawn.stderr).toContain("big.bin [");
    expect(drawn.stderr).toEndWith("\r");
    expect(drawn.stdout).toMatch(/^https:\/\/\S+\.bin\n$/);

    expect((await run(["upload", "-q", "--endpoint", server.url, path], { tty: true })).stderr).toBe("");
    expect((await run(["upload", "--endpoint", server.url, path], { tty: false })).stderr).toBe("");
  });
});

describe("upload-url and litter", () => {
  test("upload-url continues after a failure", async () => {
    const result = await run(["upload-url", "--endpoint", server.url, "https://a.example/x.gif", "https://bad.example/y"]);
    expect(result.code).toBe(1);
    expect(result.stdout).toMatch(/^https:\/\/\S+\.gif\n$/);
    expect(result.stderr).toBe("catbox: https://bad.example/y: server error: Invalid URL.\n");
  });

  test("litter sends the expiry and ignores CATBOX_USERHASH", async () => {
    const path = await file("t.zip");
    expect((await run(["litter", "--endpoint", server.url, path], { env: { CATBOX_USERHASH: "X" } })).code).toBe(0);
    expect((await run(["litter", "--endpoint", server.url, "--time=1h", path])).code).toBe(0);
    expect(server.requests.map((request) => request.fields)).toEqual([
      { reqtype: "fileupload", time: "72h" },
      { reqtype: "fileupload", time: "1h" },
    ]);
  });
});

describe("ProgressBar", () => {
  test("draws, pads over the previous line, and clears", () => {
    let output = "";
    const bar = new ProgressBar("f", (text) => (output += text));
    bar.update(0, 2048);
    bar.update(0, 2048); // unchanged: not redrawn
    bar.update(2048, 2048);
    bar.finish();
    const frames = output.split("\r");
    expect(frames[1]).toBe("f [>                       ]   0.0% 0 B/2.0 KB");
    expect(frames[2]).toBe("f [========================] 100.0% 2.0 KB/2.0 KB");
    expect(frames[3]).toBe(" ".repeat(frames[2]!.length));
    expect(frames).toHaveLength(5);
  });

  test("formats byte counts", () => {
    expect([0, 1023, 1024, 1536 * 1024, 3 * 1024 ** 3].map(formatBytes)).toEqual([
      "0 B",
      "1023 B",
      "1.0 KB",
      "1.5 MB",
      "3.0 GB",
    ]);
  });
});
