import Foundation

public enum CatboxKit {
    public static let version = "0.1.0"
}

/// Permanent uploads to https://catbox.moe (200 MB per file).
public struct CatboxClient: Sendable {
    public static let defaultEndpoint = URL(string: "https://catbox.moe/user/api.php")!

    /// Associates uploads with a catbox account so they can be managed later.
    public var userHash: String?
    public var endpoint: URL

    public init(userHash: String? = nil, endpoint: URL = CatboxClient.defaultEndpoint) {
        self.userHash = userHash
        self.endpoint = endpoint
    }

    /// Uploads a file from disk and returns its public URL.
    public func upload(fileAt fileURL: URL, progress: UploadProgressHandler? = nil) async throws -> URL {
        let filename = fileURL.lastPathComponent
        var form = baseForm(reqtype: "fileupload")
        form.append(.file(
            name: "fileToUpload", filename: filename, contentType: MIMEType.forFilename(filename), fileURL
        ))
        return try await Transport.post(form, to: endpoint, progress: progress)
    }

    /// Uploads in-memory data under the given filename and returns its public URL.
    public func upload(
        data: Data, filename: String, progress: UploadProgressHandler? = nil
    ) async throws -> URL {
        var form = baseForm(reqtype: "fileupload")
        form.append(.data(
            name: "fileToUpload", filename: filename, contentType: MIMEType.forFilename(filename), data
        ))
        return try await Transport.post(form, to: endpoint, progress: progress)
    }

    /// Asks catbox to fetch a remote URL and returns the catbox URL of the copy.
    public func upload(remoteURL: URL) async throws -> URL {
        var form = baseForm(reqtype: "urlupload")
        form.append(.field(name: "url", value: remoteURL.absoluteString))
        return try await Transport.post(form, to: endpoint, progress: nil)
    }

    private func baseForm(reqtype: String) -> MultipartForm {
        var form = MultipartForm()
        form.append(.field(name: "reqtype", value: reqtype))
        if let userHash, !userHash.isEmpty {
            form.append(.field(name: "userhash", value: userHash))
        }
        return form
    }
}
