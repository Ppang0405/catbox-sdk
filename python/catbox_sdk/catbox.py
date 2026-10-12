from os import path
from typing import BinaryIO

import requests

from catbox_sdk.base import ProgressCallback, Uploader, mimetype


class CatboxUploader(Uploader):
    """Permanent uploads to https://catbox.moe (200 MB per file)."""

    file_host_url = "https://catbox.moe/user/api.php"

    def __init__(
        self,
        user_hash: str | None = None,
        file_host_url: str = file_host_url,
        session: requests.Session | None = None,
    ) -> None:
        super().__init__(file_host_url, session)
        self.user_hash = user_hash

    def upload_from_path(self, filename: str, progress: ProgressCallback | None = None) -> str:
        """Uploads a file from disk and returns its public URL."""
        with open(filename, "rb") as file:
            return self.upload_file(path.basename(filename), file, progress)

    def upload_file(self, filename: str, file_object: BinaryIO, progress: ProgressCallback | None = None) -> str:
        """Uploads an open binary file under the given filename and returns its public URL."""
        fields = self._fields("fileupload")
        fields["fileToUpload"] = (filename, file_object, mimetype(filename))
        return self._post(fields, progress)

    def upload_from_url(self, url: str) -> str:
        """Asks catbox to fetch a remote URL and returns the catbox URL of the copy."""
        fields = self._fields("urlupload")
        fields["url"] = url
        return self._post(fields)

    def _fields(self, reqtype: str) -> dict:
        fields = {"reqtype": reqtype}
        if self.user_hash:
            fields["userhash"] = self.user_hash
        return fields
