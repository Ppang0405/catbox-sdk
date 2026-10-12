import { extname } from "node:path";

// A fixed table rather than Bun's own MIME guess (Bun.file(path).type), so every
// implementation in this repository sends the same Content-Type for the same file.
const MIME_TYPES: Readonly<Record<string, string>> = {
  "7z": "application/x-7z-compressed",
  aac: "audio/aac",
  avif: "image/avif",
  bmp: "image/bmp",
  css: "text/css",
  csv: "text/csv",
  flac: "audio/flac",
  gif: "image/gif",
  gz: "application/gzip",
  heic: "image/heic",
  htm: "text/html",
  html: "text/html",
  ico: "image/vnd.microsoft.icon",
  jpeg: "image/jpeg",
  jpg: "image/jpeg",
  js: "text/javascript",
  json: "application/json",
  m4a: "audio/mp4",
  md: "text/markdown",
  mkv: "video/x-matroska",
  mov: "video/quicktime",
  mp3: "audio/mpeg",
  mp4: "video/mp4",
  ogg: "audio/ogg",
  opus: "audio/opus",
  pdf: "application/pdf",
  png: "image/png",
  svg: "image/svg+xml",
  tar: "application/x-tar",
  tif: "image/tiff",
  tiff: "image/tiff",
  txt: "text/plain",
  wav: "audio/wav",
  webm: "video/webm",
  webp: "image/webp",
  xml: "application/xml",
  zip: "application/zip",
};

/** Guesses a Content-Type from a filename's extension, case-insensitively. */
export function mimeType(filename: string): string {
  const extension = extname(filename).slice(1).toLowerCase();
  return Object.hasOwn(MIME_TYPES, extension) ? MIME_TYPES[extension]! : "application/octet-stream";
}
