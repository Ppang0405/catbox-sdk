# catbox-sdk

Clients for [catbox.moe](https://catbox.moe) and [litterbox](https://litterbox.catbox.moe), written
in several languages as a side-by-side example of building the same library and command-line tool
in each.

Every implementation provides:

- an importable library for permanent catbox uploads (file, in-memory data, remote URL) and
  temporary litterbox uploads, and
- a `catbox` command with `upload`, `upload-url` and `litter` subcommands.

They all follow one specification, [`conformance/SPEC.md`](conformance/SPEC.md), and are checked
by the same test suite, which runs each command against a local mock server:

```bash
python3 conformance/run.py -- <command that runs the CLI>
```

## Implementations

| Language | Directory |
|---|---|
| Python | [`python/`](python/) |
| Swift | [`swift/`](swift/) |
| Go | [`go/`](go/) |
| Rust | [`rust/`](rust/) |
| TypeScript (Bun) | [`ts/`](ts/) |
| Zig | [`zig/`](zig/) |
| C | [`c/`](c/) |
| C++ | [`cpp/`](cpp/) |

Each directory has its own README with build instructions, usage and a tour of the code.

## Example

```console
$ catbox upload photo.jpg notes.txt
https://files.catbox.moe/abc123.jpg
https://files.catbox.moe/def456.txt

$ catbox litter -t 1h build.zip
https://litter.catbox.moe/ghi789.zip
```

## License

MIT
