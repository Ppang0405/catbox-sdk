"""A local stand-in for the catbox and litterbox APIs that records every request.

Responses depend on the request so tests can trigger failures:
- a file named like ``http500*`` gets HTTP 500
- a file named like ``badresp*`` gets catbox's plain-text error "No files given." with status 200
- a ``url`` field containing ``bad`` gets "Invalid URL." with status 200
- requests without Content-Length (e.g. chunked bodies) get HTTP 411
"""

import re
import threading
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


@dataclass
class FilePart:
    filename: str
    content_type: str
    data: bytes


@dataclass
class Request:
    path: str
    headers: dict
    fields: dict = field(default_factory=dict)
    files: dict = field(default_factory=dict)


def _unescape(value):
    return re.sub(r"\\(.)", r"\1", value)


def parse_multipart(content_type: str, body: bytes):
    match = re.search(r'boundary="?([^";]+)"?', content_type or "")
    if not match:
        raise ValueError(f"no boundary in Content-Type {content_type!r}")
    delimiter = b"--" + match.group(1).encode()
    fields, files = {}, {}
    chunks = body.split(delimiter)
    if len(chunks) < 3 or not chunks[-1].startswith(b"--"):
        raise ValueError("malformed multipart body")
    for chunk in chunks[1:-1]:
        if not chunk.startswith(b"\r\n") or not chunk.endswith(b"\r\n"):
            raise ValueError("malformed multipart part")
        head, sep, content = chunk[2:-2].partition(b"\r\n\r\n")
        if not sep:
            raise ValueError("multipart part without header terminator")
        headers = {}
        for line in head.decode("utf-8").split("\r\n"):
            key, _, value = line.partition(":")
            headers[key.strip().lower()] = value.strip()
        disposition = headers.get("content-disposition", "")
        name = re.search(r'\bname="((?:[^"\\]|\\.)*)"', disposition)
        filename = re.search(r'\bfilename="((?:[^"\\]|\\.)*)"', disposition)
        if not name:
            raise ValueError(f"part without name: {disposition!r}")
        if filename:
            files[_unescape(name.group(1))] = FilePart(
                _unescape(filename.group(1)), headers.get("content-type", ""), content
            )
        else:
            fields[_unescape(name.group(1))] = content.decode("utf-8")
    return fields, files


class MockServer:
    def __init__(self):
        self.requests: list[Request] = []
        self.errors: list[str] = []
        self._lock = threading.Lock()
        self._counter = 0
        server = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"  # answers `Expect: 100-continue`, which curl sends

            def do_POST(self):
                server._handle(self)

            def log_message(self, *args):
                pass

        self._httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self._httpd.daemon_threads = True
        self.port = self._httpd.server_address[1]
        self.catbox_url = f"http://127.0.0.1:{self.port}/user/api.php"
        self.litterbox_url = f"http://127.0.0.1:{self.port}/resources/internals/api.php"
        threading.Thread(target=self._httpd.serve_forever, daemon=True).start()

    def close(self):
        self._httpd.shutdown()
        self._httpd.server_close()

    def reset(self):
        with self._lock:
            self.requests.clear()
            self.errors.clear()

    def _next_id(self):
        with self._lock:
            self._counter += 1
            return f"m{self._counter:05d}"

    def _reply(self, handler, status, text):
        body = text.encode()
        handler.send_response(status)
        handler.send_header("Content-Type", "text/plain")
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)

    def _handle(self, handler):
        length = handler.headers.get("Content-Length")
        if length is None:
            with self._lock:
                self.errors.append("request without Content-Length")
            handler.close_connection = True
            return self._reply(handler, 411, "Length Required")
        body = handler.rfile.read(int(length))
        try:
            fields, files = parse_multipart(handler.headers.get("Content-Type"), body)
        except ValueError as error:
            with self._lock:
                self.errors.append(str(error))
            return self._reply(handler, 400, f"bad multipart: {error}")

        request = Request(handler.path, {k.lower(): v for k, v in handler.headers.items()}, fields, files)
        with self._lock:
            self.requests.append(request)

        host = "litter.catbox.moe" if "internals" in handler.path else "files.catbox.moe"
        upload = files.get("fileToUpload")
        if upload is not None:
            if upload.filename.startswith("http500"):
                return self._reply(handler, 500, "Internal Server Error")
            if upload.filename.startswith("badresp"):
                return self._reply(handler, 200, "No files given.")
            ext = upload.filename.rsplit(".", 1)[-1] if "." in upload.filename else "bin"
            return self._reply(handler, 200, f"https://{host}/{self._next_id()}.{ext}")
        if fields.get("reqtype") == "urlupload":
            url = fields.get("url", "")
            if "bad" in url:
                return self._reply(handler, 200, "Invalid URL.")
            ext = url.rsplit(".", 1)[-1] if "." in url.rsplit("/", 1)[-1] else "bin"
            return self._reply(handler, 200, f"https://{host}/{self._next_id()}.{ext}")
        return self._reply(handler, 200, "No request type given.")
