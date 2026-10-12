// Command catbox uploads files to catbox.moe and litterbox.catbox.moe.
//
// It prints one URL per line on stdout, reports failures on stderr, and exits 1 if any upload
// failed. See conformance/SPEC.md in the repository for the full contract.
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"net/url"
	"os"
	"path/filepath"

	catbox "github.com/Ppang0405/catbox-sdk/go"
)

const (
	exitOK      = 0
	exitFailure = 1 // an upload failed
	exitUsage   = 2 // bad command line; nothing was sent
)

const mainHelp = `Usage: catbox <command> [options] ARG...

Upload files to catbox.moe and litterbox.catbox.moe.

Commands:
  upload       upload files to catbox.moe permanently (max 200 MB each)
  upload-url   have catbox.moe fetch and store files from remote URLs
  litter       upload files to litterbox.catbox.moe temporarily (max 1 GB each)

Options:
  -h, --help   show this help
  --version    print the version

Run 'catbox <command> --help' for the options of a command.
`

// command describes a subcommand. Its options are registered on a fresh FlagSet per run.
type command struct {
	name     string
	noun     string // what the arguments are, for "no files given"
	help     string
	userHash bool // takes --user-hash
	expiry   bool // takes -t/--time
	run      func(app *app, opts *options, operands []string) bool
}

var commands = []*command{
	{
		name: "upload", noun: "files", userHash: true, run: (*app).upload,
		help: `Usage: catbox upload [--user-hash HASH] [-q] FILE...

Upload files to catbox.moe permanently (max 200 MB each).

Options:
  --user-hash HASH   your catbox user hash, so uploads show up in your account
                     (default: $CATBOX_USERHASH)
  -q, --quiet        don't show upload progress
  -h, --help         show this help
`,
	},
	{
		name: "upload-url", noun: "URLs", userHash: true, run: (*app).uploadURL,
		help: `Usage: catbox upload-url [--user-hash HASH] URL...

Have catbox.moe fetch and store files from remote URLs.

Options:
  --user-hash HASH   your catbox user hash, so uploads show up in your account
                     (default: $CATBOX_USERHASH)
  -h, --help         show this help
`,
	},
	{
		name: "litter", noun: "files", expiry: true, run: (*app).litter,
		help: `Usage: catbox litter [-t 1h|12h|24h|72h] [-q] FILE...

Upload files to litterbox.catbox.moe temporarily (max 1 GB each).

Options:
  -t, --time TIME    how long to keep the files: 1h, 12h, 24h or 72h (default: 72h)
  -q, --quiet        don't show upload progress
  -h, --help         show this help
`,
	},
}

type options struct {
	userHash string
	quiet    bool
	endpoint string // hidden; points the CLI at a test server
	expiry   catbox.Expiry
}

// app holds the process's outside world, so tests can run the CLI in-process.
type app struct {
	stdout, stderr   io.Writer
	getenv           func(string) string
	stderrIsTerminal bool
}

func main() {
	a := &app{stdout: os.Stdout, stderr: os.Stderr, getenv: os.Getenv, stderrIsTerminal: stderrIsTerminal()}
	os.Exit(a.run(os.Args[1:]))
}

// run executes the command line (without the program name) and returns the exit status.
func (a *app) run(args []string) int {
	if len(args) == 0 {
		fmt.Fprint(a.stderr, mainHelp)
		return exitUsage
	}
	switch args[0] {
	case "-h", "-help", "--help", "help":
		fmt.Fprint(a.stdout, mainHelp)
		return exitOK
	case "-version", "--version", "version":
		fmt.Fprintf(a.stdout, "catbox %s\n", catbox.Version)
		return exitOK
	}
	for _, cmd := range commands {
		if cmd.name == args[0] {
			return a.runCommand(cmd, args[1:])
		}
	}
	return a.usageError("", fmt.Sprintf("unknown command %q", args[0]))
}

func (a *app) runCommand(cmd *command, args []string) int {
	flags := flag.NewFlagSet(cmd.name, flag.ContinueOnError)
	flags.SetOutput(io.Discard) // errors and help are printed below, in our own format
	opts := &options{expiry: catbox.ThreeDays}
	if cmd.userHash {
		flags.StringVar(&opts.userHash, "user-hash", "", "")
	}
	if cmd.expiry {
		flags.Var((*expiryValue)(&opts.expiry), "t", "")
		flags.Var((*expiryValue)(&opts.expiry), "time", "")
	}
	flags.BoolVar(&opts.quiet, "q", false, "")
	flags.BoolVar(&opts.quiet, "quiet", false, "")
	flags.StringVar(&opts.endpoint, "endpoint", "", "")

	operands, err := parseInterspersed(flags, args)
	if errors.Is(err, flag.ErrHelp) {
		fmt.Fprint(a.stdout, cmd.help)
		return exitOK
	}
	if err != nil {
		return a.usageError(cmd.name, err.Error())
	}
	if len(operands) == 0 {
		return a.usageError(cmd.name, "no "+cmd.noun+" given")
	}
	if opts.endpoint != "" {
		if u, err := url.Parse(opts.endpoint); err != nil || u.Host == "" || (u.Scheme != "http" && u.Scheme != "https") {
			return a.usageError(cmd.name, fmt.Sprintf("invalid endpoint URL %q", opts.endpoint))
		}
	}
	if cmd.userHash && !isSet(flags, "user-hash") {
		opts.userHash = a.getenv("CATBOX_USERHASH")
	}

	if !cmd.run(a, opts, operands) {
		return exitFailure
	}
	return exitOK
}

func (a *app) usageError(command, message string) int {
	help := "catbox --help"
	if command != "" {
		help = "catbox " + command + " --help"
	}
	fmt.Fprintf(a.stderr, "catbox: %s\nRun '%s' for usage.\n", message, help)
	return exitUsage
}

func (a *app) upload(opts *options, files []string) bool {
	client := &catbox.Client{UserHash: opts.userHash, Endpoint: opts.endpoint}
	return a.uploadEach(files, opts.quiet, func(path string, progress catbox.ProgressFunc) (string, error) {
		return client.UploadFile(context.Background(), path, progress)
	})
}

func (a *app) uploadURL(opts *options, urls []string) bool {
	client := &catbox.Client{UserHash: opts.userHash, Endpoint: opts.endpoint}
	ok := true
	for _, u := range urls {
		link, err := client.UploadURL(context.Background(), u)
		if err != nil {
			ok = false
			a.fail(u, err)
			continue
		}
		fmt.Fprintln(a.stdout, link)
	}
	return ok
}

func (a *app) litter(opts *options, files []string) bool {
	client := &catbox.LitterboxClient{Endpoint: opts.endpoint}
	return a.uploadEach(files, opts.quiet, func(path string, progress catbox.ProgressFunc) (string, error) {
		return client.UploadFile(context.Background(), path, opts.expiry, progress)
	})
}

// uploadEach uploads files in order, printing one URL per line. It keeps going past failures
// and reports whether every upload succeeded.
func (a *app) uploadEach(paths []string, quiet bool, upload func(string, catbox.ProgressFunc) (string, error)) bool {
	ok := true
	for _, path := range paths {
		// Check up front so a missing file or a directory fails without a request.
		info, err := os.Stat(path)
		if err == nil && info.IsDir() {
			err = catbox.ErrIsDirectory
		}
		if err != nil {
			ok = false
			a.fail(path, err)
			continue
		}

		var bar *progressBar
		var progress catbox.ProgressFunc
		if !quiet && a.stderrIsTerminal {
			bar = newProgressBar(a.stderr, filepath.Base(path))
			progress = bar.update
		}
		link, err := upload(path, progress)
		bar.finish()
		if err != nil {
			ok = false
			a.fail(path, err)
			continue
		}
		fmt.Fprintln(a.stdout, link)
	}
	return ok
}

// fail reports a failed argument as "catbox: <arg>: <reason>".
func (a *app) fail(arg string, err error) {
	fmt.Fprintf(a.stderr, "catbox: %s: %s\n", arg, describe(err))
}

// describe strips the operation and path that os and net/http errors repeat,
// since the message already starts with the argument.
func describe(err error) string {
	if errors.Is(err, fs.ErrNotExist) {
		return "no such file"
	}
	var pathErr *fs.PathError
	if errors.As(err, &pathErr) {
		return pathErr.Err.Error()
	}
	var urlErr *url.Error
	if errors.As(err, &urlErr) {
		return urlErr.Err.Error()
	}
	return err.Error()
}

// parseInterspersed parses flags that may appear before, between or after operands
// (the flag package alone stops at the first operand). Everything after "--" is an operand.
func parseInterspersed(flags *flag.FlagSet, args []string) ([]string, error) {
	var operands []string
	for {
		if err := flags.Parse(args); err != nil {
			return nil, err
		}
		rest := flags.Args()
		if len(rest) == 0 {
			return operands, nil
		}
		if consumed := len(args) - len(rest); consumed > 0 && args[consumed-1] == "--" {
			return append(operands, rest...), nil
		}
		operands = append(operands, rest[0])
		args = rest[1:]
	}
}

func isSet(flags *flag.FlagSet, name string) bool {
	set := false
	flags.Visit(func(f *flag.Flag) {
		if f.Name == name {
			set = true
		}
	})
	return set
}

// expiryValue is a flag.Value that only accepts litterbox's expiry times.
type expiryValue catbox.Expiry

func (e *expiryValue) String() string { return string(*e) }

func (e *expiryValue) Set(s string) error {
	expiry, err := catbox.ParseExpiry(s)
	if err != nil {
		return errors.New("must be 1h, 12h, 24h or 72h")
	}
	*e = expiryValue(expiry)
	return nil
}
