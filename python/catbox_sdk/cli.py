"""The `catbox` command. See conformance/SPEC.md for the behavior shared by every implementation."""

import argparse
import os
import sys
from collections.abc import Callable

from catbox_sdk._version import __version__
from catbox_sdk.base import ProgressCallback
from catbox_sdk.catbox import CatboxUploader
from catbox_sdk.errors import CatboxError
from catbox_sdk.litterbox import EXPIRY_TIMES, LitterboxUploader


class ProgressBar:
    """A single-line progress bar on stderr, so stdout stays clean for piping URLs."""

    def __init__(self, label: str) -> None:
        self.label = label
        self.last_rendered = -1
        self.line_length = 0

    def update(self, sent: int, total: int) -> None:
        if total <= 0:
            return
        permille = min(sent, total) * 1000 // total
        if permille == self.last_rendered:
            return
        self.last_rendered = permille
        width = 24
        filled = permille * width // 1000
        bar = ("=" * filled + (">" if filled < width else "")).ljust(width)
        line = f"{self.label} [{bar}] {permille / 10:5.1f}% {_format_bytes(sent)}/{_format_bytes(total)}"
        # Pad over the previous line instead of using ANSI erase codes, which older Windows consoles lack.
        sys.stderr.write("\r" + line.ljust(self.line_length))
        sys.stderr.flush()
        self.line_length = len(line)

    def finish(self) -> None:
        if self.line_length:
            sys.stderr.write("\r" + " " * self.line_length + "\r")
            sys.stderr.flush()


def _format_bytes(count: int) -> str:
    value, unit = float(count), 0
    units = ["B", "KB", "MB", "GB"]
    while value >= 1024 and unit < len(units) - 1:
        value /= 1024
        unit += 1
    return f"{count} B" if unit == 0 else f"{value:.1f} {units[unit]}"


def _error(item: str, message: str) -> None:
    print(f"catbox: {item}: {message}", file=sys.stderr)


def _upload_each(paths: list[str], quiet: bool, upload: Callable[[str, ProgressCallback | None], str]) -> bool:
    """Uploads each file in turn, printing one URL per line. Returns False if any upload failed."""
    ok = True
    for file_path in paths:
        if os.path.isdir(file_path):
            ok = False
            _error(file_path, "is a directory")
            continue
        if not os.path.isfile(file_path):
            ok = False
            _error(file_path, "no such file")
            continue

        bar = None if quiet or not sys.stderr.isatty() else ProgressBar(os.path.basename(file_path))
        try:
            url = upload(file_path, bar.update if bar else None)
        except (CatboxError, OSError) as error:
            ok = False
            if bar:
                bar.finish()
            _error(file_path, str(error))
            continue
        if bar:
            bar.finish()
        print(url, flush=True)
    return ok


def _user_hash(args: argparse.Namespace) -> str | None:
    return args.user_hash if args.user_hash is not None else os.environ.get("CATBOX_USERHASH")


def _run_upload(args: argparse.Namespace) -> bool:
    uploader = CatboxUploader(_user_hash(args), args.endpoint or CatboxUploader.file_host_url)
    return _upload_each(args.files, args.quiet, uploader.upload_from_path)


def _run_upload_url(args: argparse.Namespace) -> bool:
    uploader = CatboxUploader(_user_hash(args), args.endpoint or CatboxUploader.file_host_url)
    ok = True
    for url in args.urls:
        try:
            print(uploader.upload_from_url(url), flush=True)
        except CatboxError as error:
            ok = False
            _error(url, str(error))
    return ok


def _run_litter(args: argparse.Namespace) -> bool:
    uploader = LitterboxUploader(args.endpoint or LitterboxUploader.file_host_url)
    return _upload_each(
        args.files, args.quiet, lambda file_path, progress: uploader.upload_from_path(file_path, args.time, progress)
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="catbox", description="Upload files to catbox.moe and litterbox.catbox.moe.")
    parser.add_argument("--version", action="version", version=__version__)
    commands = parser.add_subparsers(dest="command", required=True, metavar="<command>")

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("-q", "--quiet", action="store_true", help="don't show upload progress")
    common.add_argument("--endpoint", help=argparse.SUPPRESS)

    user_hash = argparse.ArgumentParser(add_help=False)
    user_hash.add_argument(
        "--user-hash", help="your catbox user hash, so uploads show up in your account (default: $CATBOX_USERHASH)"
    )

    upload = commands.add_parser(
        "upload", parents=[user_hash, common], help="upload files to catbox.moe permanently (max 200 MB each)"
    )
    upload.add_argument("files", nargs="+", metavar="FILE", help="files to upload")
    upload.set_defaults(run=_run_upload)

    upload_url = commands.add_parser(
        "upload-url", parents=[user_hash, common], help="have catbox.moe fetch and store files from remote URLs"
    )
    upload_url.add_argument("urls", nargs="+", metavar="URL", help="URLs to copy to catbox")
    upload_url.set_defaults(run=_run_upload_url)

    litter = commands.add_parser(
        "litter", parents=[common], help="upload files to litterbox.catbox.moe temporarily (max 1 GB each)"
    )
    litter.add_argument("-t", "--time", choices=EXPIRY_TIMES, default="72h", help="how long to keep the files (default: 72h)")
    litter.add_argument("files", nargs="+", metavar="FILE", help="files to upload")
    litter.set_defaults(run=_run_litter)
    return parser


def main(argv: list[str] | None = None) -> None:
    args = build_parser().parse_args(argv)
    sys.exit(0 if args.run(args) else 1)


if __name__ == "__main__":
    main()
