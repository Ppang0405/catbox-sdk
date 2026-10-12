from collections.abc import Callable
from os import path

import requests
from requests_toolbelt import MultipartEncoder, MultipartEncoderMonitor

from catbox_sdk._version import __version__
from catbox_sdk.errors import CatboxError

ProgressCallback = Callable[[int, int], None]
"""Called with (bytes sent, total bytes) while a request body is uploaded."""

_MIME_TYPES = {
    "7z": "application/x-7z-compressed",
    "aac": "audio/aac",
    "avif": "image/avif",
    "bmp": "image/bmp",
    "css": "text/css",
    "csv": "text/csv",
    "flac": "audio/flac",
    "gif": "image/gif",
    "gz": "application/gzip",
    "heic": "image/heic",
    "htm": "text/html",
    "html": "text/html",
    "ico": "image/vnd.microsoft.icon",
    "jpeg": "image/jpeg",
    "jpg": "image/jpeg",
    "js": "text/javascript",
    "json": "application/json",
    "m4a": "audio/mp4",
    "md": "text/markdown",
    "mkv": "video/x-matroska",
    "mov": "video/quicktime",
    "mp3": "audio/mpeg",
    "mp4": "video/mp4",
    "ogg": "audio/ogg",
    "opus": "audio/opus",
    "pdf": "application/pdf",
    "png": "image/png",
    "svg": "image/svg+xml",
    "tar": "application/x-tar",
    "tif": "image/tiff",
    "tiff": "image/tiff",
    "txt": "text/plain",
    "wav": "audio/wav",
    "webm": "video/webm",
    "webp": "image/webp",
    "xml": "application/xml",
    "zip": "application/zip",
}


def mimetype(filename: str) -> str:
    """Guesses a Content-Type from the file extension, case-insensitively.

    A fixed table rather than the `mimetypes` module, which reads system files and so
    answers differently per platform.
    """
    extension = path.splitext(filename)[1].lstrip(".").lower()
    return _MIME_TYPES.get(extension, "application/octet-stream")


class Uploader:
    user_agent = f"catbox-python/{__version__}"

    def __init__(self, file_host_url: str, session: requests.Session | None = None) -> None:
        self.file_host_url = file_host_url
        self.session = session or requests.Session()

    def _post(self, fields: dict, progress: ProgressCallback | None = None) -> str:
        """Posts a multipart form and returns the URL catbox answers with.

        The encoder streams file parts and knows its total length, so the request is sent
        with a Content-Length header rather than chunked.
        """
        body = MultipartEncoder(fields=fields)
        if progress is not None:
            body = MultipartEncoderMonitor(body, callback=lambda monitor: progress(monitor.bytes_read, monitor.len))
        try:
            response = self.session.post(
                self.file_host_url,
                data=body,
                headers={"Content-Type": body.content_type, "User-Agent": self.user_agent},
            )
        except requests.RequestException as error:
            raise CatboxError(str(error)) from error

        text = response.text.strip()
        if not response.ok:
            message = f"server returned HTTP {response.status_code}"
            raise CatboxError(f"{message}: {text}" if text else message, response.status_code)
        # Catbox reports most errors as plain text with status 200.
        if not text.startswith(("https://", "http://")):
            raise CatboxError(f"server error: {text}" if text else "server returned an empty response")
        return text
