import { expect, test } from "bun:test";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";

import { escapeQuoted, MultipartForm, openFile } from "../src/multipart.ts";

const decoder = new TextDecoder();

test("encodes fields and files in order", async () => {
  const form = new MultipartForm()
    .field("reqtype", "fileupload")
    .file("fileToUpload", "a.txt", "text/plain", new Blob(["hello"]));
  const b = form.boundary;
  expect(decoder.decode(await form.bytes())).toBe(
    `--${b}\r\nContent-Disposition: form-data; name="reqtype"\r\n\r\nfileupload\r\n` +
      `--${b}\r\nContent-Disposition: form-data; name="fileToUpload"; filename="a.txt"\r\n` +
      `Content-Type: text/plain\r\n\r\nhello\r\n` +
      `--${b}--\r\n`,
  );
  expect(form.contentType).toBe(`multipart/form-data; boundary=${b}`);
});

test("length matches the encoded bytes, counting UTF-8 and binary content", async () => {
  const form = new MultipartForm()
    .field("name", "café \u{1F431}")
    .file("f", "猫.bin", "application/octet-stream", new Blob([new Uint8Array(70_000).fill(7)]));
  expect((await form.bytes()).byteLength).toBe(form.length);
});

test("progress counts up to the total", async () => {
  const form = new MultipartForm().file("f", "x", "application/octet-stream", new Blob([new Uint8Array(200_000)]));
  const calls: [number, number][] = [];
  await new Response(form.stream((sent, total) => calls.push([sent, total]))).arrayBuffer();
  expect(calls.length).toBeGreaterThan(1);
  expect(calls.every(([, total]) => total === form.length)).toBe(true);
  expect(calls.map(([sent]) => sent)).toEqual(calls.map(([sent]) => sent).toSorted((a, b) => a - b));
  expect(calls.at(-1)).toEqual([form.length, form.length]);
});

test("escapes quotes and backslashes and drops line breaks in names", () => {
  expect(escapeQuoted('a"b\\c\r\nd')).toBe('a\\"b\\\\cd');
});

test("streams files from disk and fails if a file shrinks", async () => {
  const dir = await mkdtemp(join(tmpdir(), "catbox-test-"));
  try {
    const path = join(dir, "shrink.txt");
    await writeFile(path, "longer than it will be");
    const { filename, content } = await openFile(path);
    expect(filename).toBe("shrink.txt");
    const form = new MultipartForm().file("f", filename, "text/plain", content);
    // The size, and so Content-Length, was fixed when the part was added.
    await writeFile(path, "short");
    await expect(new Response(form.stream()).arrayBuffer()).rejects.toThrow(/changed size/);
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test("openFile rejects directories and missing files", async () => {
  await expect(openFile(tmpdir())).rejects.toThrow(/not a regular file/);
  await expect(openFile(join(tmpdir(), "catbox-definitely-missing"))).rejects.toMatchObject({ code: "ENOENT" });
});
