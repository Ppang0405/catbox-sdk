#!/usr/bin/env python3
"""Check a catbox CLI against SPEC.md using a local mock server.

Usage:
    python3 conformance/run.py -- ./go/catbox
    python3 conformance/run.py -- bun run ts/src/cli.ts
    python3 conformance/run.py -k litter -- ./rust/target/debug/catbox
"""

import argparse
import hashlib
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import traceback
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mock_server import MockServer  # noqa: E402

VERSION = "0.1.0"


@dataclass
class Result:
    code: int
    stdout: str
    stderr: str

    @property
    def lines(self):
        return [line.strip() for line in self.stdout.splitlines() if line.strip()]

    def __str__(self):
        return f"exit={self.code}\n--- stdout ---\n{self.stdout}--- stderr ---\n{self.stderr}"


class Failure(AssertionError):
    pass


def check(condition, message, result=None):
    if not condition:
        raise Failure(message + (f"\n{result}" if result is not None else ""))


class Context:
    def __init__(self, command, server, workdir):
        self.command = command
        self.server = server
        self.workdir = workdir

    def run(self, *args, env=None):
        environment = {k: v for k, v in os.environ.items() if k != "CATBOX_USERHASH"}
        environment.update(env or {})
        process = subprocess.run(
            [*self.command, *args],
            stdin=subprocess.DEVNULL,
            capture_output=True,
            env=environment,
            timeout=120,
        )
        return Result(
            process.returncode,
            process.stdout.decode("utf-8", "replace"),
            process.stderr.decode("utf-8", "replace"),
        )

    def file(self, name, data=None, size=None):
        path = Path(self.workdir, name)
        path.parent.mkdir(parents=True, exist_ok=True)
        if data is None:
            data = os.urandom(size if size is not None else 2048)
        path.write_bytes(data if isinstance(data, bytes) else data.encode())
        return str(path)

    def upload(self, *args, env=None):
        return self.run("upload", "--endpoint", self.server.catbox_url, *args, env=env)

    def upload_url(self, *args, env=None):
        return self.run("upload-url", "--endpoint", self.server.catbox_url, *args, env=env)

    def litter(self, *args, env=None):
        return self.run("litter", "--endpoint", self.server.litterbox_url, *args, env=env)


TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def assert_file_request(request, path, content_type, result):
    part = request.files.get("fileToUpload")
    check(part is not None, "request has no fileToUpload part", result)
    check(part.filename == Path(path).name, f"filename was {part.filename!r}, expected {Path(path).name!r}", result)
    check(part.content_type == content_type, f"Content-Type was {part.content_type!r}, expected {content_type!r}", result)
    expected = Path(path).read_bytes()
    check(
        hashlib.sha256(part.data).digest() == hashlib.sha256(expected).digest(),
        f"uploaded bytes differ ({len(part.data)} received, {len(expected)} on disk)",
        result,
    )


def assert_user_agent(request, result):
    agent = request.headers.get("user-agent", "")
    check(agent.startswith("catbox-"), f"User-Agent {agent!r} does not start with 'catbox-'", result)


def assert_no_userhash(request, result):
    check(request.fields.get("userhash", "") == "", f"unexpected userhash {request.fields.get('userhash')!r}", result)


@test
def version(ctx):
    result = ctx.run("--version")
    check(result.code == 0, "--version should exit 0", result)
    check(VERSION in result.stdout, f"--version should print {VERSION} on stdout", result)


@test
def help(ctx):
    result = ctx.run("--help")
    check(result.code == 0, "--help should exit 0", result)
    for command in ("upload", "upload-url", "litter"):
        check(command in result.stdout + result.stderr, f"--help should mention {command!r}", result)


@test
def upload_single_file(ctx):
    path = ctx.file("photo.jpg")
    result = ctx.upload("--user-hash", "HASH123", path)
    check(result.code == 0, "upload should exit 0", result)
    check(len(ctx.server.requests) == 1, f"expected 1 request, got {len(ctx.server.requests)}", result)
    request = ctx.server.requests[0]
    check(request.path == "/user/api.php", f"posted to {request.path}", result)
    check(request.fields.get("reqtype") == "fileupload", f"reqtype was {request.fields.get('reqtype')!r}", result)
    check(request.fields.get("userhash") == "HASH123", f"userhash was {request.fields.get('userhash')!r}", result)
    assert_file_request(request, path, "image/jpeg", result)
    assert_user_agent(request, result)
    check(result.lines == [result.lines[0]] and result.lines[0].startswith("https://files.catbox.moe/"),
          "stdout should be exactly the returned URL", result)
    check(result.stderr == "", "a successful run with non-TTY stderr should write nothing to stderr", result)


@test
def upload_userhash_from_environment(ctx):
    path = ctx.file("a.png")
    result = ctx.upload(path, env={"CATBOX_USERHASH": "ENVHASH"})
    check(result.code == 0, "upload should exit 0", result)
    check(ctx.server.requests[0].fields.get("userhash") == "ENVHASH", "userhash should come from CATBOX_USERHASH", result)


@test
def upload_userhash_flag_overrides_environment(ctx):
    path = ctx.file("a.png")
    result = ctx.upload("--user-hash", "FLAG", path, env={"CATBOX_USERHASH": "ENVHASH"})
    check(result.code == 0, "upload should exit 0", result)
    check(ctx.server.requests[0].fields.get("userhash") == "FLAG", "--user-hash should win over CATBOX_USERHASH", result)


@test
def upload_without_userhash(ctx):
    result = ctx.upload(ctx.file("a.gif"))
    check(result.code == 0, "upload should exit 0", result)
    assert_no_userhash(ctx.server.requests[0], result)
    assert_file_request(ctx.server.requests[0], Path(ctx.workdir, "a.gif"), "image/gif", result)


@test
def upload_quiet_flag(ctx):
    for flag in ("-q", "--quiet"):
        ctx.server.reset()
        result = ctx.upload(flag, ctx.file("q.txt"))
        check(result.code == 0, f"upload {flag} should exit 0", result)
        check(len(result.lines) == 1, f"upload {flag} should print one URL", result)


@test
def upload_multiple_files_continues_after_failure(ctx):
    first = ctx.file("notes.txt", "hello")
    missing = str(Path(ctx.workdir, "missing.png"))
    second = ctx.file("UPPER.JPG")
    third = ctx.file("data.unknownext")
    result = ctx.upload(first, missing, second, third)
    check(result.code == 1, "a failed file should make the exit status 1", result)
    check(len(ctx.server.requests) == 3,
          f"expected 3 requests (none for the missing file), got {len(ctx.server.requests)}", result)
    check(len(result.lines) == 3 and all(line.startswith("https://") for line in result.lines),
          "stdout should hold the 3 URLs and nothing else", result)
    check([line.rsplit(".", 1)[-1] for line in result.lines] == ["txt", "JPG", "unknownext"],
          "URLs should be printed in argument order", result)
    check("missing.png" in result.stderr, "stderr should name the missing file", result)
    assert_file_request(ctx.server.requests[0], first, "text/plain", result)
    assert_file_request(ctx.server.requests[1], second, "image/jpeg", result)
    assert_file_request(ctx.server.requests[2], third, "application/octet-stream", result)


@test
def upload_mime_types(ctx):
    expected = {
        "a.jpeg": "image/jpeg", "a.webp": "image/webp", "a.json": "application/json",
        "a.pdf": "application/pdf", "a.zip": "application/zip", "a.mp4": "video/mp4",
        "noextension": "application/octet-stream",
    }
    paths = [ctx.file(name) for name in expected]
    result = ctx.upload(*paths)
    check(result.code == 0, "upload should exit 0", result)
    for request, path in zip(ctx.server.requests, paths):
        assert_file_request(request, path, expected[Path(path).name], result)


@test
def upload_sends_base_name_only(ctx):
    path = ctx.file(os.path.join("nested", "dir", "my photo.png"))
    result = ctx.upload(path)
    check(result.code == 0, "upload should exit 0", result)
    assert_file_request(ctx.server.requests[0], path, "image/png", result)


@test
def upload_large_file(ctx):
    path = ctx.file("big.bin", size=24 * 1024 * 1024 + 7)
    result = ctx.upload(path)
    check(result.code == 0, "upload should exit 0", result)
    assert_file_request(ctx.server.requests[0], path, "application/octet-stream", result)


@test
def upload_empty_file(ctx):
    path = ctx.file("empty.txt", b"")
    result = ctx.upload(path)
    check(result.code == 0, "upload should exit 0", result)
    assert_file_request(ctx.server.requests[0], path, "text/plain", result)


@test
def upload_directory_fails_without_request(ctx):
    directory = Path(ctx.workdir, "somedir")
    directory.mkdir()
    result = ctx.upload(str(directory))
    check(result.code == 1, "uploading a directory should exit 1", result)
    check(not ctx.server.requests, "no request should be sent for a directory", result)
    check(result.stdout.strip() == "", "stdout should be empty", result)
    check("somedir" in result.stderr, "stderr should name the directory", result)


@test
def upload_server_error_text(ctx):
    result = ctx.upload(ctx.file("badresp.txt"))
    check(result.code == 1, "an error body should exit 1", result)
    check(result.stdout.strip() == "", "error text must not be printed as a URL", result)
    check("No files given." in result.stderr, "stderr should include the server's message", result)


@test
def upload_http_error_status(ctx):
    result = ctx.upload(ctx.file("http500.txt"))
    check(result.code == 1, "HTTP 500 should exit 1", result)
    check(result.stdout.strip() == "", "stdout should be empty", result)
    check("500" in result.stderr, "stderr should mention the status code", result)


@test
def upload_connection_refused(ctx):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    result = ctx.run("upload", "--endpoint", f"http://127.0.0.1:{port}/user/api.php", ctx.file("a.txt"))
    check(result.code == 1, "a network error should exit 1", result)
    check(result.stdout.strip() == "", "stdout should be empty", result)
    check(result.stderr.strip() != "", "stderr should explain the failure", result)


@test
def upload_without_arguments_is_usage_error(ctx):
    result = ctx.upload()
    check(result.code >= 2, "no files should be a usage error (exit >= 2)", result)
    check(not ctx.server.requests, "usage errors must not send requests", result)


@test
def unknown_option_is_usage_error(ctx):
    result = ctx.upload("--no-such-option", ctx.file("a.txt"))
    check(result.code >= 2, "an unknown option should be a usage error (exit >= 2)", result)
    check(not ctx.server.requests, "usage errors must not send requests", result)


@test
def upload_url(ctx):
    result = ctx.upload_url("--user-hash", "HASH", "https://example.com/image.png")
    check(result.code == 0, "upload-url should exit 0", result)
    request = ctx.server.requests[0]
    check(request.fields.get("reqtype") == "urlupload", f"reqtype was {request.fields.get('reqtype')!r}", result)
    check(request.fields.get("url") == "https://example.com/image.png", f"url was {request.fields.get('url')!r}", result)
    check(request.fields.get("userhash") == "HASH", f"userhash was {request.fields.get('userhash')!r}", result)
    check(not request.files, "upload-url should not send a file part", result)
    assert_user_agent(request, result)
    check(len(result.lines) == 1 and result.lines[0].endswith(".png"), "stdout should be the returned URL", result)


@test
def upload_url_continues_after_failure(ctx):
    result = ctx.upload_url("https://example.com/a.gif", "https://bad.example/b", "https://example.com/c.webp")
    check(result.code == 1, "a failed URL should make the exit status 1", result)
    check(len(ctx.server.requests) == 3, "every URL should be sent", result)
    check(len(result.lines) == 2 and result.lines[0].endswith(".gif") and result.lines[1].endswith(".webp"),
          "stdout should hold the 2 successful URLs in order", result)
    check("Invalid URL." in result.stderr, "stderr should include the server's message", result)
    check("bad.example" in result.stderr, "stderr should name the failed URL", result)
    for request in ctx.server.requests:
        assert_no_userhash(request, result)


@test
def litter_default_expiry(ctx):
    path = ctx.file("temp.zip")
    result = ctx.litter(path, env={"CATBOX_USERHASH": "IGNORED"})
    check(result.code == 0, "litter should exit 0", result)
    request = ctx.server.requests[0]
    check(request.path == "/resources/internals/api.php", f"posted to {request.path}", result)
    check(request.fields.get("reqtype") == "fileupload", f"reqtype was {request.fields.get('reqtype')!r}", result)
    check(request.fields.get("time") == "72h", f"time was {request.fields.get('time')!r}, expected '72h'", result)
    assert_no_userhash(request, result)
    assert_file_request(request, path, "application/zip", result)
    assert_user_agent(request, result)
    check(len(result.lines) == 1 and result.lines[0].startswith("https://litter.catbox.moe/"),
          "stdout should be the returned URL", result)
    check(result.stderr == "", "a successful run with non-TTY stderr should write nothing to stderr", result)


@test
def litter_expiry_options(ctx):
    cases = ((["-t", "1h"], "1h"), (["--time", "12h"], "12h"), (["-t", "24h"], "24h"), (["--time", "72h"], "72h"))
    for args, expected in cases:
        ctx.server.reset()
        result = ctx.litter(*args, ctx.file("t.txt"))
        check(result.code == 0, f"litter {' '.join(args)} should exit 0", result)
        check(ctx.server.requests[0].fields.get("time") == expected, f"time should be {expected!r}", result)


@test
def litter_invalid_expiry_is_usage_error(ctx):
    result = ctx.litter("-t", "5h", ctx.file("t.txt"))
    check(result.code >= 2, "an invalid --time should be a usage error (exit >= 2)", result)
    check(not ctx.server.requests, "usage errors must not send requests", result)


@test
def litter_missing_file(ctx):
    good = ctx.file("ok.txt")
    result = ctx.litter(str(Path(ctx.workdir, "nope.txt")), good)
    check(result.code == 1, "a missing file should exit 1", result)
    check(len(ctx.server.requests) == 1 and len(result.lines) == 1, "the existing file should still be uploaded", result)
    check("nope.txt" in result.stderr, "stderr should name the missing file", result)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("-k", dest="filter", help="only run tests whose name contains this text")
    parser.add_argument("command", nargs=argparse.REMAINDER, help="command that runs the CLI, after --")
    options = parser.parse_args()
    command = options.command[1:] if options.command[:1] == ["--"] else options.command
    if not command:
        parser.error("give the command to test after --")
    if shutil.which(command[0]) is None and not Path(command[0]).exists():
        parser.error(f"command not found: {command[0]}")

    server = MockServer()
    passed, failed = 0, []
    try:
        for fn in TESTS:
            if options.filter and options.filter not in fn.__name__:
                continue
            server.reset()
            with tempfile.TemporaryDirectory(prefix="catbox-conformance-") as workdir:
                try:
                    fn(Context(command, server, workdir))
                    check(not server.errors, "server rejected a request: " + "; ".join(server.errors))
                except Failure as error:
                    failed.append(fn.__name__)
                    print(f"FAIL {fn.__name__}\n  " + str(error).replace("\n", "\n  "))
                    continue
                except Exception:
                    failed.append(fn.__name__)
                    print(f"ERROR {fn.__name__}\n" + traceback.format_exc())
                    continue
            passed += 1
            print(f"ok   {fn.__name__}")
    finally:
        server.close()

    print(f"\n{passed} passed, {len(failed)} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
