# catbox-sdk (Python)

[![PyPI](https://img.shields.io/pypi/v/catbox-sdk.svg)](https://pypi.python.org/pypi/catbox-sdk)
![Python Versions](https://img.shields.io/pypi/pyversions/catbox-sdk.svg)
![License](https://img.shields.io/github/license/ppang0405/catbox-sdk.svg)

Upload files to [catbox.moe](https://catbox.moe) and [litterbox](https://litterbox.catbox.moe),
as a library or with the `catbox` command. Requires Python 3.12+.

```bash
python3 -m pip install catbox-sdk
```

## CLI

```bash
catbox upload photo.jpg video.mp4               # permanent, prints one URL per line
catbox upload --user-hash YOUR_HASH photo.jpg   # or set CATBOX_USERHASH
catbox upload-url https://example.com/image.png
catbox litter -t 1h build.zip                   # temporary: 1h, 12h, 24h or 72h (default)
```

`python -m catbox_sdk` runs the same command. The behavior is shared with the other
implementations in this repository; see [`conformance/SPEC.md`](../conformance/SPEC.md).

## Library

```python
from catbox_sdk import CatboxError, CatboxUploader, LitterboxUploader

uploader = CatboxUploader()  # or CatboxUploader(user_hash="...")

url = uploader.upload_from_path("photo.jpg")
url = uploader.upload_file("notes.txt", open("notes.txt", "rb"))
url = uploader.upload_from_url("https://example.com/image.png")

url = LitterboxUploader().upload_from_path("build.zip", time="1h")

try:
    uploader.upload_from_path("huge.iso", progress=lambda sent, total: print(sent, total))
except CatboxError as error:
    print("upload failed:", error, error.status_code)
```

Every method returns the full file URL and raises `CatboxError` when catbox rejects the upload or
cannot be reached. Files are streamed rather than read into memory.

## Development

```bash
cd python
poetry install
poetry run pytest
python3 ../conformance/run.py -- .venv/bin/catbox   # with POETRY_VIRTUALENVS_IN_PROJECT=true
```
