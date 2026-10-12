import { mimeType } from "./mime.ts";
import { MultipartForm, openFile, toBlob, type UploadData } from "./multipart.ts";
import { post, type UploadOptions } from "./transport.ts";

/** How long litterbox keeps a file before deleting it. */
export const EXPIRY_TIMES = ["1h", "12h", "24h", "72h"] as const;

export type Expiry = (typeof EXPIRY_TIMES)[number];

export function isExpiry(value: string): value is Expiry {
  return (EXPIRY_TIMES as readonly string[]).includes(value);
}

export interface LitterboxUploadOptions extends UploadOptions {
  /** Defaults to `"72h"`. */
  expiry?: Expiry | undefined;
}

export interface LitterboxClientOptions {
  /** The API URL. Defaults to {@link LitterboxClient.DEFAULT_ENDPOINT}. */
  endpoint?: string | URL | undefined;
}

/** Temporary uploads to https://litterbox.catbox.moe (1 GB per file). */
export class LitterboxClient {
  static readonly DEFAULT_ENDPOINT = "https://litterbox.catbox.moe/resources/internals/api.php";

  readonly endpoint: string;

  constructor(options: LitterboxClientOptions = {}) {
    this.endpoint = new URL(options.endpoint ?? LitterboxClient.DEFAULT_ENDPOINT).href;
  }

  /** Uploads a file from disk, streaming it, and returns its temporary public URL. */
  async uploadFile(path: string, options: LitterboxUploadOptions = {}): Promise<string> {
    const expiry = checkExpiry(options.expiry);
    const { filename, content } = await openFile(path);
    return this.#upload(filename, content, expiry, options);
  }

  /** Uploads in-memory content under the given filename and returns its temporary public URL. */
  async uploadData(data: UploadData, filename: string, options: LitterboxUploadOptions = {}): Promise<string> {
    return this.#upload(filename, toBlob(data), checkExpiry(options.expiry), options);
  }

  #upload(filename: string, content: Blob, expiry: Expiry, options: UploadOptions): Promise<string> {
    const form = new MultipartForm()
      .field("reqtype", "fileupload")
      .field("time", expiry)
      .file("fileToUpload", filename, mimeType(filename), content);
    return post(this.endpoint, form, options);
  }
}

// The type already restricts expiry, but JavaScript callers bypass it.
function checkExpiry(expiry: string = "72h"): Expiry {
  if (!isExpiry(expiry)) {
    throw new RangeError(`expiry must be one of ${EXPIRY_TIMES.join(", ")}, not ${JSON.stringify(expiry)}`);
  }
  return expiry;
}
