import ArgumentParser
import CatboxKit
import Foundation

@main
struct CatboxCommand: AsyncParsableCommand {
    static let configuration = CommandConfiguration(
        commandName: "catbox",
        abstract: "Upload files to catbox.moe and litterbox.catbox.moe.",
        version: CatboxKit.version,
        subcommands: [Upload.self, UploadURL.self, Litter.self]
    )
}

struct CommonOptions: ParsableArguments {
    @Flag(name: .shortAndLong, help: "Don't show upload progress.")
    var quiet = false

    @Option(help: .hidden)
    var endpoint: String?

    func endpointURL(default defaultURL: URL) throws -> URL {
        guard let endpoint else { return defaultURL }
        guard let url = URL(string: endpoint), url.scheme != nil else {
            throw ValidationError("Invalid endpoint URL: \(endpoint)")
        }
        return url
    }
}

extension CatboxCommand {
    struct Upload: AsyncParsableCommand {
        static let configuration = CommandConfiguration(
            abstract: "Upload files to catbox.moe permanently (max 200 MB each)."
        )

        @Option(help: "Your catbox user hash, so uploads show up in your account. Defaults to $CATBOX_USERHASH.")
        var userHash: String?

        @OptionGroup var options: CommonOptions

        @Argument(help: "Files to upload.", completion: .file())
        var files: [String]

        func run() async throws {
            let client = CatboxClient(
                userHash: userHash ?? ProcessInfo.processInfo.environment["CATBOX_USERHASH"],
                endpoint: try options.endpointURL(default: CatboxClient.defaultEndpoint)
            )
            try await uploadEach(files, quiet: options.quiet) { fileURL, progress in
                try await client.upload(fileAt: fileURL, progress: progress)
            }
        }
    }

    struct UploadURL: AsyncParsableCommand {
        static let configuration = CommandConfiguration(
            commandName: "upload-url",
            abstract: "Have catbox.moe fetch and store files from remote URLs."
        )

        @Option(help: "Your catbox user hash, so uploads show up in your account. Defaults to $CATBOX_USERHASH.")
        var userHash: String?

        @OptionGroup var options: CommonOptions

        @Argument(help: "URLs to copy to catbox.")
        var urls: [String]

        func run() async throws {
            let client = CatboxClient(
                userHash: userHash ?? ProcessInfo.processInfo.environment["CATBOX_USERHASH"],
                endpoint: try options.endpointURL(default: CatboxClient.defaultEndpoint)
            )
            var failed = false
            for string in urls {
                do {
                    guard let url = URL(string: string), url.scheme != nil else {
                        throw ValidationError("not a valid URL")
                    }
                    print(try await client.upload(remoteURL: url).absoluteString)
                } catch {
                    failed = true
                    Terminal.error("\(string): \(describe(error))")
                }
            }
            if failed { throw ExitCode.failure }
        }
    }

    struct Litter: AsyncParsableCommand {
        static let configuration = CommandConfiguration(
            abstract: "Upload files to litterbox.catbox.moe temporarily (max 1 GB each)."
        )

        @Option(
            name: .shortAndLong,
            help: "How long to keep the files: 1h, 12h, 24h or 72h.",
            transform: { value in
                guard let expiry = LitterboxClient.Expiry(rawValue: value) else {
                    throw ValidationError("Expiry must be one of 1h, 12h, 24h or 72h.")
                }
                return expiry
            }
        )
        var time: LitterboxClient.Expiry = .threeDays

        @OptionGroup var options: CommonOptions

        @Argument(help: "Files to upload.", completion: .file())
        var files: [String]

        func run() async throws {
            let client = LitterboxClient(endpoint: try options.endpointURL(default: LitterboxClient.defaultEndpoint))
            try await uploadEach(files, quiet: options.quiet) { fileURL, progress in
                try await client.upload(fileAt: fileURL, expiry: time, progress: progress)
            }
        }
    }
}

/// Uploads each file in turn, printing one URL per line on stdout. Keeps going past
/// failures and exits non-zero at the end if any upload failed.
private func uploadEach(
    _ paths: [String], quiet: Bool,
    upload: (URL, UploadProgressHandler?) async throws -> URL
) async throws {
    var failed = false
    for path in paths {
        let fileURL = URL(fileURLWithPath: path)
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: fileURL.path, isDirectory: &isDirectory), !isDirectory.boolValue else {
            failed = true
            Terminal.error("\(path): no such file")
            continue
        }

        let bar = (quiet || !Terminal.stderrIsInteractive) ? nil : ProgressBar(label: fileURL.lastPathComponent)
        do {
            let progress: UploadProgressHandler? = bar.map { bar in { @Sendable sent, total in bar.update(sent: sent, total: total) } }
            let url = try await upload(fileURL, progress)
            bar?.finish()
            print(url.absoluteString)
        } catch {
            bar?.finish()
            failed = true
            Terminal.error("\(path): \(describe(error))")
        }
    }
    if failed { throw ExitCode.failure }
}

private func describe(_ error: Error) -> String {
    (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
}
