# catbox_sdk

[![PyPI](https://img.shields.io/pypi/v/catbox-sdk.svg)](https://pypi.python.org/pypi/catbox-sdk)
![Python Versions](https://img.shields.io/pypi/pyversions/catbox-sdk.svg)
![License](https://img.shields.io/github/license/ppang0405/catbox-sdk.svg)

Installation

```bash
$ python3 -m pip install catbox-sdk
```

## Example
```python
from catbox_sdk import CatboxUploader

uploader = CatboxUploader()

filename = "./test_img1.jpg"
uploader.upload_from_path(filename)

uploader.upload_file("test_img2.jpg", open("./test_img2.jpg", "rb"))

uploader.upload_from_url(image_url)
```
