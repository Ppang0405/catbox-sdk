import { mimeType } from "./mime.ts";
import { MultipartForm, openFile, toBlob, type UploadData } from "./multipart.ts";
import { post, type UploadOptions } from "./transport.ts";

export interface CatboxClientOptions {
  /** Associates uploads with a catbox account so they can be managed later. */
  userHash?: string | undefined;
  /** The API URL. Defaults to {@link CatboxClient.DEFAULT_ENDPOINT}. */
  endpoint?: string | URL | undefined;
}

/** Permanent uploads to https://catbox.moe (200 MB per file). */
export class CatboxClient {
  static readonly DEFAULT_ENDPOINT = "https://catbox.moe/user/api.php";

  readonly userHash: string | undefined;
  readonly endpoint: string;

  constructor(options: CatboxClientOptions = {}) {
    this.userHash = options.userHash || undefined;
    this.endpoint = new URL(options.endpoint ?? CatboxClient.DEFAULT_ENDPOINT).href;
  }

  /** Uploads a file from disk, streaming it, and returns its public URL. */
  async uploadFile(path: string, options: UploadOptions = {}): Promise<string> {
    const { filename, content } = await openFile(path);
    return this.#upload(filename, content, options);
  }

  /** Uploads in-memory content under the given filename and returns its public URL. */
  async uploadData(data: UploadData, filename: string, options: UploadOptions = {}): Promise<string> {
    return this.#upload(filename, toBlob(data), options);
  }

  /** Asks catbox to fetch a remote URL and returns the catbox URL of the copy. */
  async uploadUrl(url: string | URL, options: Pick<UploadOptions, "signal"> = {}): Promise<string> {
    const form = this.#form("urlupload").field("url", String(url));
    return post(this.endpoint, form, options);
  }

  #upload(filename: string, content: Blob, options: UploadOptions): Promise<string> {
    const form = this.#form("fileupload").file("fileToUpload", filename, mimeType(filename), content);
    return post(this.endpoint, form, options);
  }

  #form(reqtype: string): MultipartForm {
    const form = new MultipartForm().field("reqtype", reqtype);
    return this.userHash ? form.field("userhash", this.userHash) : form;
  }
}
