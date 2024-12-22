from catbox_sdk.base import Uploader


class LitterboxUploader(Uploader):
    __slots__ = ()

    file_host_url = "https://litterbox.catbox.moe/resources/internals/api.php"

    def __init__(self):
        super().__init__(self.file_host_url)

    def upload_from_path(self, filename, time: str = "72h"):
        file = open(filename, "rb")
        try:
            data = {
                "reqtype": "fileupload",
                "fileToUpload": (file.name, file, self._mimetype(filename)),
                "time": time,  # 1h, 12h, 24h, and 72h.
            }
            response = self._multipart_post(data)
        finally:
            file.close()

        return response.text
