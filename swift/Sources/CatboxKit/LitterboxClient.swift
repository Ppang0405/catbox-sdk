import Foundation

/// Temporary uploads to https://litterbox.catbox.moe (1 GB per file).
public struct LitterboxClient: Sendable {
    public static let defaultEndpoint = URL(string: "https://litterbox.catbox.moe/resources/internals/api.php")!

    /// How long litterbox keeps the file before deleting it.
    public enum Expiry: String, CaseIterable, Sendable {
        case oneHour = "1h"
        case twelveHours = "12h"
        case oneDay = "24h"
        case threeDays = "72h"
    }

    public var endpoint: URL

    public init(endpoint: URL = LitterboxClient.defaultEndpoint) {
        self.endpoint = endpoint
    }

    /// Uploads a file from disk and returns its temporary public URL.
    public func upload(
        fileAt fileURL: URL, expiry: Expiry = .threeDays, progress: UploadProgressHandler? = nil
    ) async throws -> URL {
        let filename = fileURL.lastPathComponent
        var form = MultipartForm()
        form.append(.field(name: "reqtype", value: "fileupload"))
        form.append(.field(name: "time", value: expiry.rawValue))
        form.append(.file(
            name: "fileToUpload", filename: filename, contentType: MIMEType.forFilename(filename), fileURL
        ))
        return try await Transport.post(form, to: endpoint, progress: progress)
    }
}
