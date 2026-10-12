import { expect, test } from "bun:test";

import { mimeType } from "../src/index.ts";

test.each([
  ["photo.jpg", "image/jpeg"],
  ["photo.JPEG", "image/jpeg"],
  ["a.png", "image/png"],
  ["a.gif", "image/gif"],
  ["a.webp", "image/webp"],
  ["notes.txt", "text/plain"],
  ["data.json", "application/json"],
  ["doc.pdf", "application/pdf"],
  ["archive.zip", "application/zip"],
  ["clip.MP4", "video/mp4"],
  ["backup.tar.gz", "application/gzip"],
  ["dir.d/file.7z", "application/x-7z-compressed"],
])("%s is %s", (filename, expected) => {
  expect(mimeType(filename)).toBe(expected);
});

test.each(["noextension", "data.unknownext", ".bashrc", "trailing.", "toString", "a.constructor"])(
  "%s falls back to application/octet-stream",
  (filename) => {
    expect(mimeType(filename)).toBe("application/octet-stream");
  },
);
