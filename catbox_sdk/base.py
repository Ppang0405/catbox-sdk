import mimetypes
import sys
from os import path

import requests
from requests_toolbelt import MultipartEncoder, MultipartEncoderMonitor


class Uploader:
    __slots__ = ("file_host_url",)

    requests_session = requests.Session()

    def __init__(self, file_host_url):
        self.file_host_url = file_host_url

    @classmethod
    def _progress_bar(cls, monitor):
        progress = int(monitor.bytes_read / monitor.len * 20)
        sys.stdout.write("\r[{}/{}] bytes |".format(monitor.bytes_read, monitor.len))
        sys.stdout.write("{}>".format("=" * progress))
        sys.stdout.write("{}|".format(" " * (20 - progress)))
        sys.stdout.flush()

    def _multipart_post(self, data):
        encoder = MultipartEncoder(fields=data)
        monitor = MultipartEncoderMonitor(encoder, callback=self._progress_bar)
        r = self.requests_session.post(
            self.file_host_url, data=monitor, headers={"Content-Type": monitor.content_type}
        )
        return r

    def _mimetype(self, filename):
        _, extension = path.splitext(filename)
        if extension == "":
            extension = ".txt"
        mimetypes.init()
        try:
            return mimetypes.types_map[extension]
        except KeyError:
            return "plain/text"

    def upload_from_url(self):
        raise NotImplementedError()

    def upload_from_path(self):
        raise NotImplementedError()

    def upload_file(self):
        raise NotImplementedError()
