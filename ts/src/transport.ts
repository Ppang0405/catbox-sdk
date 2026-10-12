import { CatboxError } from "./errors.ts";
import type { MultipartForm, ProgressCallback } from "./multipart.ts";
import { USER_AGENT } from "./version.ts";

export interface UploadOptions {
  /** Called as the request body is sent, with bytes sent so far and the total body size. */
  progress?: ProgressCallback | undefined;
  /** Cancels the upload; the returned promise then rejects with the signal's reason. */
  signal?: AbortSignal | undefined;
}

/** Posts a multipart form and returns the URL catbox answers with. */
export async function post(endpoint: string, form: MultipartForm, options: UploadOptions = {}): Promise<string> {
  // fetch would send a ReadableStream body chunked, which catbox rejects. Bun keeps an
  // explicit Content-Length for stream bodies, so the file is streamed from disk in
  // known-length form while every chunk is counted for the progress callback.
  // (A Blob of [header, Bun.file(path), footer] would also have a known length, but
  // Bun 1.4 drops the file's bytes when a BunFile is nested inside another Blob.)
  let bodyError: unknown;
  const body = rememberError(form.stream(options.progress), (error) => (bodyError = error));
  let response: Response;
  try {
    response = await fetch(endpoint, {
      method: "POST",
      headers: {
        "Content-Type": form.contentType,
        "Content-Length": String(form.length),
        "User-Agent": USER_AGENT,
      },
      body,
      signal: options.signal ?? null,
    });
  } catch (error) {
    if (bodyError !== undefined) throw bodyError; // e.g. the file could not be read
    if (options.signal?.aborted) throw error;
    // Bun's network errors carry a short code (ECONNREFUSED, ConnectionClosed, ...).
    const code = (error as { code?: unknown } | null)?.code;
    const reason = typeof code === "string" ? code : error instanceof Error ? error.message : String(error);
    throw new CatboxError(`could not reach ${new URL(endpoint).host} (${reason})`, { cause: error });
  }
  return parseResponse(response.status, await response.text());
}

/** Turns a catbox reply into the uploaded file's URL, or throws its error. */
export function parseResponse(status: number, body: string): string {
  const text = body.trim();
  if (status < 200 || status > 299) {
    const message = `server returned HTTP ${status}`;
    throw new CatboxError(text ? `${message}: ${text}` : message, { status });
  }
  // Catbox reports most errors as plain text with status 200.
  if (!text.startsWith("https://") && !text.startsWith("http://")) {
    throw new CatboxError(text ? `server error: ${text}` : "server returned an empty response");
  }
  return text;
}

/**
 * Passes `stream` through, reporting an error it fails with. fetch rejects with that same
 * error, and this tells it apart from a network failure so it isn't relabelled as one.
 */
function rememberError(stream: ReadableStream<Uint8Array>, onError: (error: unknown) => void): ReadableStream<Uint8Array> {
  const reader = stream.getReader();
  return new ReadableStream<Uint8Array>({
    async pull(controller) {
      try {
        const { done, value } = await reader.read();
        if (done) controller.close();
        else controller.enqueue(value);
      } catch (error) {
        onError(error);
        controller.error(error);
      }
    },
    cancel: (reason) => reader.cancel(reason),
  });
}
