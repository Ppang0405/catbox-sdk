/** Thrown when catbox rejects an upload or cannot be reached. */
export class CatboxError extends Error {
  /** The HTTP status code, when the server answered with a non-2xx status. */
  readonly status: number | undefined;

  constructor(message: string, options: { status?: number; cause?: unknown } = {}) {
    super(message, { cause: options.cause });
    this.name = "CatboxError";
    this.status = options.status;
  }
}
