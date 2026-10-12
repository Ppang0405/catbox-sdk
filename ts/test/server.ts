/** A local catbox stand-in for tests: records each request and answers like the real API. */

export interface RecordedRequest {
  path: string;
  headers: Headers;
  /** Content-Length as sent, or null when the body was chunked. */
  contentLength: string | null;
  bodyLength: number;
  fields: Record<string, string>;
  files: Record<string, File>;
  /** Each file part's Content-Type exactly as sent (Bun's parser appends a charset to text types). */
  fileTypes: Record<string, string>;
}

export interface TestServer {
  url: string;
  requests: RecordedRequest[];
  stop(): void;
}

export function startServer(): TestServer {
  const requests: RecordedRequest[] = [];
  let counter = 0;
  const server = Bun.serve({
    port: 0,
    hostname: "127.0.0.1",
    async fetch(request) {
      const contentLength = request.headers.get("content-length");
      const body = await request.arrayBuffer();
      const form = await new Response(body, { headers: { "content-type": request.headers.get("content-type") ?? "" } }).formData();
      const fields: Record<string, string> = {};
      const files: Record<string, File> = {};
      for (const [name, value] of form) {
        if (typeof value === "string") fields[name] = value;
        else files[name] = value;
      }
      const raw = Buffer.from(body).toString("latin1");
      const fileTypes: Record<string, string> = {};
      for (const [, name, type] of raw.matchAll(/name="([^"]*)"; filename="[^"]*"\r\nContent-Type: ([^\r]*)/g)) {
        fileTypes[name!] = type!;
      }
      requests.push({
        path: new URL(request.url).pathname,
        headers: request.headers,
        contentLength,
        bodyLength: body.byteLength,
        fields,
        files,
        fileTypes,
      });

      if (contentLength === null) return new Response("Length Required", { status: 411 });
      const file = files.fileToUpload;
      if (file?.name.startsWith("http500")) return new Response("Internal Server Error", { status: 500 });
      if (file?.name.startsWith("badresp")) return new Response("No files given.");
      if (fields.url?.includes("bad")) return new Response("Invalid URL.");
      const name = file?.name ?? fields.url ?? "";
      const extension = name.includes(".") ? name.slice(name.lastIndexOf(".") + 1) : "bin";
      counter += 1;
      return new Response(`https://files.catbox.moe/t${counter}.${extension}\n`);
    },
  });
  return { url: `http://127.0.0.1:${server.port}/user/api.php`, requests, stop: () => server.stop(true) };
}
