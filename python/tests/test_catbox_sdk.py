import io

import pytest

from catbox_sdk import CatboxError, CatboxUploader, LitterboxUploader
from catbox_sdk.base import mimetype


def test_upload_from_path(server, tmp_path):
    file = tmp_path / "photo.jpg"
    file.write_bytes(b"\xff\xd8 jpeg bytes")

    url = CatboxUploader("HASH", server.catbox_url).upload_from_path(str(file))

    assert url.startswith("https://files.catbox.moe/") and url.endswith(".jpg")
    request = server.requests[0]
    assert request.fields == {"reqtype": "fileupload", "userhash": "HASH"}
    part = request.files["fileToUpload"]
    assert (part.filename, part.content_type, part.data) == ("photo.jpg", "image/jpeg", b"\xff\xd8 jpeg bytes")
    assert request.headers["user-agent"].startswith("catbox-python/")
    assert "content-length" in request.headers


def test_upload_file_object_without_user_hash(server):
    url = CatboxUploader(file_host_url=server.catbox_url).upload_file("notes.txt", io.BytesIO(b"hello"))

    assert url.endswith(".txt")
    request = server.requests[0]
    assert request.fields == {"reqtype": "fileupload"}
    assert request.files["fileToUpload"].data == b"hello"


def test_upload_from_url(server):
    url = CatboxUploader(file_host_url=server.catbox_url).upload_from_url("https://example.com/a.png")

    assert url.endswith(".png")
    assert server.requests[0].fields == {"reqtype": "urlupload", "url": "https://example.com/a.png"}


def test_progress_reports_whole_body(server):
    calls = []
    CatboxUploader(file_host_url=server.catbox_url).upload_file(
        "a.bin", io.BytesIO(b"x" * 100_000), progress=lambda sent, total: calls.append((sent, total))
    )

    assert calls and calls[-1][0] == calls[-1][1]


def test_error_text_raises(server):
    with pytest.raises(CatboxError, match="No files given."):
        CatboxUploader(file_host_url=server.catbox_url).upload_file("badresp.txt", io.BytesIO(b""))


def test_http_error_raises_with_status(server):
    with pytest.raises(CatboxError) as error:
        CatboxUploader(file_host_url=server.catbox_url).upload_file("http500.txt", io.BytesIO(b""))
    assert error.value.status_code == 500


def test_connection_error_raises_catbox_error():
    with pytest.raises(CatboxError):
        CatboxUploader(file_host_url="http://127.0.0.1:9/").upload_from_url("https://example.com/a.png")


def test_litterbox_upload(server, tmp_path):
    file = tmp_path / "temp.zip"
    file.write_bytes(b"zip")

    url = LitterboxUploader(server.litterbox_url).upload_from_path(str(file), time="1h")

    assert url.startswith("https://litter.catbox.moe/")
    assert server.requests[0].fields == {"reqtype": "fileupload", "time": "1h"}
    assert server.requests[0].files["fileToUpload"].content_type == "application/zip"


def test_litterbox_rejects_unknown_time(server, tmp_path):
    with pytest.raises(ValueError):
        LitterboxUploader(server.litterbox_url).upload_from_path(str(tmp_path / "x"), time="5h")
    assert not server.requests


@pytest.mark.parametrize(
    ("filename", "expected"),
    [
        ("photo.JPG", "image/jpeg"),
        ("notes.txt", "text/plain"),
        ("archive.unknown", "application/octet-stream"),
        ("noextension", "application/octet-stream"),
    ],
)
def test_mimetype(filename, expected):
    assert mimetype(filename) == expected
