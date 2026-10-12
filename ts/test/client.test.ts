import { afterAll, beforeEach, describe, expect, test } from "bun:test";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { CatboxClient, CatboxError, LitterboxClient, USER_AGENT } from "../src/index.ts";
import { parseResponse } from "../src/transport.ts";
import { startServer } from "./server.ts";

const server = startServer();
const dir = await mkdtemp(join(tmpdir(), "catbox-test-"));

afterAll(async () => {
  server.stop();
  await rm(dir, { recursive: true, force: true });
});

beforeEach(() => {
  server.requests.length = 0;
});

async function tempFile(name: string, content: string | Uint8Array): Promise<string> {
  const path = join(dir, name);
  await writeFile(path, content);
  return path;
}

describe("CatboxClient", () => {
  test("uploads a file with Content-Length, base name, MIME type and user hash", async () => {
    const bytes = crypto.getRandomValues(new Uint8Array(300_000));
    const path = await tempFile("Photo.JPG", bytes);
    const progress: number[] = [];
    const client = new CatboxClient({ userHash: "HASH", endpoint: server.url });

    const url = await client.uploadFile(path, { progress: (sent) => progress.push(sent) });

    expect(url).toMatch(/^https:\/\/files\.catbox\.moe\/t\d+\.JPG$/);
    expect(server.requests).toHaveLength(1);
    const request = server.requests[0]!;
    expect(request.path).toBe("/user/api.php");
    expect(request.contentLength).toBe(String(request.bodyLength));
    expect(request.headers.get("user-agent")).toBe(USER_AGENT);
    expect(request.fields).toEqual({ reqtype: "fileupload", userhash: "HASH" });
    const file = request.files.fileToUpload!;
    expect(file.name).toBe("Photo.JPG");
    expect(request.fileTypes.fileToUpload).toBe("image/jpeg");
    expect(new Uint8Array(await file.arrayBuffer())).toEqual(bytes);
    expect(progress.at(-1)).toBe(request.bodyLength);
  });

  test("leaves out an empty user hash", async () => {
    await new CatboxClient({ userHash: "", endpoint: server.url }).uploadFile(await tempFile("a.txt", "x"));
    expect(server.requests[0]!.fields).toEqual({ reqtype: "fileupload" });
  });

  test("uploads in-memory data", async () => {
    const client = new CatboxClient({ endpoint: server.url });
    expect(await client.uploadData(new TextEncoder().encode("hi"), "a.json")).toEndWith(".json");
    expect(await client.uploadData(new Blob(["there"]), "b.txt")).toEndWith(".txt");
    expect(await client.uploadData(new ArrayBuffer(3), "noext")).toEndWith(".bin");
    const [first, second, third] = server.requests;
    expect(first!.fileTypes.fileToUpload).toBe("application/json");
    expect(await first!.files.fileToUpload!.text()).toBe("hi");
    expect(await second!.files.fileToUpload!.text()).toBe("there");
    expect(third!.fileTypes.fileToUpload).toBe("application/octet-stream");
  });

  test("asks catbox to fetch a URL", async () => {
    const client = new CatboxClient({ userHash: "H", endpoint: server.url });
    expect(await client.uploadUrl(new URL("https://example.com/cat.png"))).toEndWith(".png");
    expect(server.requests[0]!.fields).toEqual({ reqtype: "urlupload", userhash: "H", url: "https://example.com/cat.png" });
    expect(server.requests[0]!.files).toEqual({});
  });

  test("turns plain-text errors and HTTP errors into CatboxError", async () => {
    const client = new CatboxClient({ endpoint: server.url });
    const textError = await client.uploadUrl("https://bad.example/x").catch((error: unknown) => error);
    expect(textError).toBeInstanceOf(CatboxError);
    expect(textError).toMatchObject({ message: "server error: Invalid URL.", status: undefined });

    const httpError = await client.uploadData(new Uint8Array(1), "http500.txt").catch((error: unknown) => error);
    expect(httpError).toMatchObject({ message: "server returned HTTP 500: Internal Server Error", status: 500 });
  });

  test("reports an unreachable server as CatboxError", async () => {
    const probe = Bun.serve({ port: 0, fetch: () => new Response() });
    const port = probe.port;
    probe.stop(true);
    const client = new CatboxClient({ endpoint: `http://127.0.0.1:${port}/` });
    await expect(client.uploadUrl("https://example.com/a.png")).rejects.toBeInstanceOf(CatboxError);
  });

  test("rejects missing files without sending a request", async () => {
    const client = new CatboxClient({ endpoint: server.url });
    await expect(client.uploadFile(join(dir, "missing.png"))).rejects.toMatchObject({ code: "ENOENT" });
    await expect(client.uploadFile(dir)).rejects.toThrow(/not a regular file/);
    expect(server.requests).toHaveLength(0);
  });

  test("can be cancelled", async () => {
    const client = new CatboxClient({ endpoint: server.url });
    const promise = client.uploadData(new Uint8Array(10), "a.bin", { signal: AbortSignal.abort() });
    await expect(promise).rejects.toMatchObject({ name: "AbortError" });
  });

  test("rejects an invalid endpoint", () => {
    expect(() => new CatboxClient({ endpoint: "not a url" })).toThrow(TypeError);
  });
});

describe("LitterboxClient", () => {
  test("sends the expiry, defaulting to 72h, and no user hash", async () => {
    const client = new LitterboxClient({ endpoint: server.url });
    const path = await tempFile("temp.zip", "zip");
    await client.uploadFile(path);
    await client.uploadFile(path, { expiry: "1h" });
    await client.uploadData(new Uint8Array(4), "mem.bin", { expiry: "12h" });
    expect(server.requests.map((request) => request.fields)).toEqual([
      { reqtype: "fileupload", time: "72h" },
      { reqtype: "fileupload", time: "1h" },
      { reqtype: "fileupload", time: "12h" },
    ]);
    expect(server.requests[0]!.fileTypes.fileToUpload).toBe("application/zip");
  });

  test("rejects an unknown expiry before sending anything", async () => {
    const client = new LitterboxClient({ endpoint: server.url });
    // @ts-expect-error: JavaScript callers can pass any string.
    await expect(client.uploadData(new Uint8Array(1), "a.txt", { expiry: "5h" })).rejects.toThrow(RangeError);
    expect(server.requests).toHaveLength(0);
  });
});

describe("parseResponse", () => {
  test("accepts http and https URLs, trimmed", () => {
    expect(parseResponse(200, " https://files.catbox.moe/a.png\n")).toBe("https://files.catbox.moe/a.png");
    expect(parseResponse(201, "http://x/y")).toBe("http://x/y");
  });

  test("rejects other bodies and statuses", () => {
    expect(() => parseResponse(200, "")).toThrow("server returned an empty response");
    expect(() => parseResponse(200, "No files given.")).toThrow("server error: No files given.");
    expect(() => parseResponse(412, "")).toThrow("server returned HTTP 412");
    expect(() => parseResponse(404, "https://looks.like/a-url")).toThrow(CatboxError);
  });
});
