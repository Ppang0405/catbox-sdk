import { stat } from "node:fs/promises";
import { basename } from "node:path";

/** Called as the request body is sent, with bytes sent so far and the total body size. */
export type ProgressCallback = (sent: number, total: number) => void;

/** In-memory content accepted by the `uploadData` methods. */
export type UploadData = Blob | Uint8Array | ArrayBuffer;

type Part =
  | { kind: "field"; name: string; value: string }
  | { kind: "file"; name: string; filename: string; contentType: string; content: Blob; size: number };

const encoder = new TextEncoder();
const CRLF = encoder.encode("\r\n");

/**
 * A multipart/form-data body whose length is known before it is sent.
 *
 * File parts are Blobs, and `Bun.file(path)` is a Blob that reads from disk lazily, so
 * `stream()` never holds more than one chunk of a file in memory however large it is.
 */
export class MultipartForm {
  readonly boundary = `catbox-${crypto.randomUUID()}`;
  readonly #parts: Part[] = [];

  get contentType(): string {
    return `multipart/form-data; boundary=${this.boundary}`;
  }

  field(name: string, value: string): this {
    this.#parts.push({ kind: "field", name, value });
    return this;
  }

  file(name: string, filename: string, contentType: string, content: Blob): this {
    // The size is read once: it fixes Content-Length, and the stream checks the file still matches.
    this.#parts.push({ kind: "file", name, filename, contentType, content, size: content.size });
    return this;
  }

  /** The exact number of bytes `stream()` produces, sent as Content-Length. */
  get length(): number {
    let total = this.#closing().byteLength;
    for (const part of this.#parts) {
      total += this.#header(part).byteLength + this.#contentLength(part) + CRLF.byteLength;
    }
    return total;
  }

  /** The encoded body. `progress` is called after each chunk is handed to the reader. */
  stream(progress?: ProgressCallback): ReadableStream<Uint8Array> {
    const total = this.length;
    const chunks = this.#chunks();
    let sent = 0;
    return new ReadableStream<Uint8Array>({
      async pull(controller) {
        const { done, value } = await chunks.next();
        if (done) {
          controller.close();
          return;
        }
        controller.enqueue(value);
        sent += value.byteLength;
        progress?.(sent, total);
      },
      async cancel() {
        await chunks.return(undefined);
      },
    });
  }

  /** The whole body in memory. Meant for tests and small forms. */
  async bytes(): Promise<Uint8Array> {
    return new Uint8Array(await new Response(this.stream()).arrayBuffer());
  }

  async *#chunks(): AsyncGenerator<Uint8Array> {
    for (const part of this.#parts) {
      yield this.#header(part);
      if (part.kind === "field") {
        yield encoder.encode(part.value);
      } else {
        // Content-Length was computed from the size up front; a file that changed since
        // would produce a malformed request, so fail instead.
        let read = 0;
        for await (const chunk of part.content.stream()) {
          read += chunk.byteLength;
          if (read > part.size) break;
          yield chunk;
        }
        if (read !== part.size) {
          throw new Error(`${part.filename} changed size during upload (${part.size} bytes expected, ${read} read)`);
        }
      }
      yield CRLF;
    }
    yield this.#closing();
  }

  #header(part: Part): Uint8Array {
    let header = `--${this.boundary}\r\nContent-Disposition: form-data; name="${escapeQuoted(part.name)}"`;
    if (part.kind === "file") {
      header += `; filename="${escapeQuoted(part.filename)}"\r\nContent-Type: ${part.contentType}`;
    }
    return encoder.encode(`${header}\r\n\r\n`);
  }

  #contentLength(part: Part): number {
    return part.kind === "field" ? encoder.encode(part.value).byteLength : part.size;
  }

  #closing(): Uint8Array {
    return encoder.encode(`--${this.boundary}--\r\n`);
  }
}

/** Quotes and line breaks would break out of the header, so escape or drop them. */
export function escapeQuoted(value: string): string {
  return value.replaceAll("\\", "\\\\").replaceAll('"', '\\"').replace(/[\r\n]/g, "");
}

/** A file on disk as a lazily read Blob, plus the base name to upload it under. */
export async function openFile(path: string): Promise<{ filename: string; content: Blob }> {
  const info = await stat(path);
  if (!info.isFile()) {
    throw new Error(`${path} is not a regular file`);
  }
  return { filename: basename(path), content: Bun.file(path) };
}

export function toBlob(data: UploadData): Blob {
  return data instanceof Blob ? data : new Blob([data as Uint8Array<ArrayBuffer> | ArrayBuffer]);
}
