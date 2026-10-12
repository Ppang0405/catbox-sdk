import Foundation
#if os(Windows)
import ucrt
#elseif canImport(Glibc)
import Glibc
#elseif canImport(Musl)
import Musl
#else
import Darwin
#endif

enum Terminal {
    static var stderrIsInteractive: Bool {
        #if os(Windows)
        return _isatty(2) != 0
        #else
        return isatty(2) != 0
        #endif
    }

    static func error(_ message: String) {
        writeStderr("catbox: \(message)\n")
    }

    static func writeStderr(_ string: String) {
        FileHandle.standardError.write(Data(string.utf8))
    }
}

/// A single-line progress bar on stderr, so stdout stays clean for piping URLs.
final class ProgressBar: @unchecked Sendable {
    private let label: String
    private let lock = NSLock()
    private var lastRendered = -1
    private var lineLength = 0

    init(label: String) {
        self.label = label
    }

    func update(sent: Int64, total: Int64) {
        guard total > 0 else { return }
        let permille = Int(min(sent, total) * 1000 / total)
        lock.lock()
        defer { lock.unlock() }
        // Redraw at most once per 0.1% so fast uploads don't flood the terminal.
        guard permille != lastRendered else { return }
        lastRendered = permille

        let width = 24
        let filled = permille * width / 1000
        let bar = String(repeating: "=", count: filled) + (filled < width ? ">" : "")
            + String(repeating: " ", count: max(0, width - filled - 1))
        let percent = String(format: "%5.1f%%", Double(permille) / 10)
        let line = "\(label) [\(bar)] \(percent) \(Self.format(sent))/\(Self.format(total))"
        // Pad over the previous line instead of using ANSI erase codes, which older Windows consoles lack.
        Terminal.writeStderr("\r" + line + String(repeating: " ", count: max(0, lineLength - line.count)))
        lineLength = line.count
    }

    func finish() {
        lock.lock()
        defer { lock.unlock() }
        if lineLength > 0 { Terminal.writeStderr("\r" + String(repeating: " ", count: lineLength) + "\r") }
    }

    private static func format(_ bytes: Int64) -> String {
        let units = ["B", "KB", "MB", "GB"]
        var value = Double(bytes)
        var unit = 0
        while value >= 1024, unit < units.count - 1 {
            value /= 1024
            unit += 1
        }
        return unit == 0 ? "\(bytes) B" : String(format: "%.1f", value) + " " + units[unit]
    }
}
