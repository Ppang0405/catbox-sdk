package main

import (
	"fmt"
	"io"
	"os"
	"strings"
	"sync"
	"unicode/utf8"
)

// stderrIsTerminal reports whether stderr is a terminal. Terminals are character devices on
// Unix and consoles are character devices on Windows, so this needs no cgo, syscalls or x/term.
// (It is also true for /dev/null and NUL, where drawing progress is harmless.)
func stderrIsTerminal() bool {
	info, err := os.Stderr.Stat()
	return err == nil && info.Mode()&os.ModeCharDevice != 0
}

// progressBar draws a single-line progress bar on stderr, so stdout stays clean for piping URLs.
// update is called from net/http's goroutine and finish from the main one, hence the mutex.
type progressBar struct {
	mu           sync.Mutex
	w            io.Writer
	label        string
	lastRendered int // in tenths of a percent
	lineLength   int // in runes
	done         bool
}

func newProgressBar(w io.Writer, label string) *progressBar {
	return &progressBar{w: w, label: label, lastRendered: -1}
}

func (b *progressBar) update(sent, total int64) {
	if total <= 0 {
		return
	}
	permille := int(min(sent, total) * 1000 / total)

	b.mu.Lock()
	defer b.mu.Unlock()
	// Redraw at most once per 0.1% so fast uploads don't flood the terminal.
	if b.done || permille == b.lastRendered {
		return
	}
	b.lastRendered = permille

	const width = 24
	filled := permille * width / 1000
	bar := strings.Repeat("=", filled)
	if filled < width {
		bar += ">"
	}
	line := fmt.Sprintf("%s [%-*s] %5.1f%% %s/%s",
		b.label, width, bar, float64(permille)/10, formatBytes(sent), formatBytes(total))
	length := utf8.RuneCountInString(line)
	// Pad over the previous line instead of using ANSI erase codes, which older Windows consoles lack.
	fmt.Fprint(b.w, "\r"+line+strings.Repeat(" ", max(0, b.lineLength-length)))
	b.lineLength = length
}

// finish erases the bar. A nil bar does nothing, so callers needn't check for quiet mode.
func (b *progressBar) finish() {
	if b == nil {
		return
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.lineLength > 0 && !b.done {
		fmt.Fprint(b.w, "\r"+strings.Repeat(" ", b.lineLength)+"\r")
	}
	b.done = true
}

func formatBytes(count int64) string {
	units := []string{"B", "KB", "MB", "GB"}
	value, unit := float64(count), 0
	for value >= 1024 && unit < len(units)-1 {
		value /= 1024
		unit++
	}
	if unit == 0 {
		return fmt.Sprintf("%d B", count)
	}
	return fmt.Sprintf("%.1f %s", value, units[unit])
}
