from catbox_sdk import CatboxUploader

uploader = CatboxUploader()


def test_catbox_uploader_from_path():
    filename = "./test_img1.jpg"
    assert isinstance(uploader.upload_from_path(filename), str)


def test_catbox_uploader_file_object():
    assert isinstance(uploader.upload_file("test_img2.jpg", open("./test_img2.jpg", "rb")), str)
