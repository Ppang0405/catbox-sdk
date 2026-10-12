from os import path

import requests

from catbox_sdk.base import ProgressCallback, Uploader, mimetype

EXPIRY_TIMES = ("1h", "12h", "24h", "72h")


class LitterboxUploader(Uploader):
    """Temporary uploads to https://litterbox.catbox.moe (1 GB per file)."""

    file_host_url = "https://litterbox.catbox.moe/resources/internals/api.php"

    def __init__(self, file_host_url: str = file_host_url, session: requests.Session | None = None) -> None:
        super().__init__(file_host_url, session)

    def upload_from_path(self, filename: str, time: str = "72h", progress: ProgressCallback | None = None) -> str:
        """Uploads a file from disk, kept for `time` (1h, 12h, 24h or 72h), and returns its URL."""
        if time not in EXPIRY_TIMES:
            raise ValueError(f"time must be one of {', '.join(EXPIRY_TIMES)}, not {time!r}")
        name = path.basename(filename)
        with open(filename, "rb") as file:
            fields = {
                "reqtype": "fileupload",
                "time": time,
                "fileToUpload": (name, file, mimetype(name)),
            }
            return self._post(fields, progress)
