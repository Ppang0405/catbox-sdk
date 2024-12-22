class CatboxException(Exception):
    def __init__(self, *args: object) -> None:
        super().__init__(*args)
        raise NotImplementedError()
