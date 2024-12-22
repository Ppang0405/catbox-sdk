from catbox_sdk.base import Uploader


class CatboxUploader(Uploader):
    __slots__ = ("user_hash",)

    file_host_url = "https://catbox.moe/user/api.php"

    def __init__(self, user_hash=""):
        super().__init__(self.file_host_url)
        self.user_hash = user_hash

    def upload_from_path(self, filename):
        file = open(filename, "rb")
        try:
            data = {
                "reqtype": "fileupload",
                "userhash": self.user_hash,
                "fileToUpload": (file.name, file, self._mimetype(filename)),
            }
            response = self._multipart_post(data)
        finally:
            file.close()

        return response.text

    def upload_from_url(self, url):
        data = {"reqtype": "urlupload", "userhash": self.user_hash, "url": url}

        response = self._multipart_post(data)
        return response.text.split("/")[-1]

    def upload_file(self, filename, file_object):
        payload = {"reqtype": "fileupload", "userhash": self.user_hash}

        files = [("fileToUpload", (filename, file_object))]

        response = self.requests_session.request(
            "POST", self.file_host_url, data=payload, files=files
        )
        return response.text.split("/")[-1]
