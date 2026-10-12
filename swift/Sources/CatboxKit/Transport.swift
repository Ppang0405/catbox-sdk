import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif

/// Called as the request body is sent, with bytes sent so far and the total body size.
public typealias UploadProgressHandler = @Sendable (_ sent: Int64, _ total: Int64) -> Void

enum Transport {
    static func post(_ form: MultipartForm, to endpoint: URL, progress: UploadProgressHandler?) async throws -> URL {
        var request = URLRequest(url: endpoint)
        request.httpMethod = "POST"
        request.setValue(form.contentType, forHTTPHeaderField: "Content-Type")
        request.setValue("catbox-swift/\(CatboxKit.version)", forHTTPHeaderField: "User-Agent")

        let body: UploadBody
        if form.containsFiles {
            let tempURL = FileManager.default.temporaryDirectory
                .appendingPathComponent("catbox-upload-\(UUID().uuidString)")
            try form.write(to: tempURL)
            body = .file(tempURL)
        } else {
            body = .data(try form.encodedData())
        }
        defer {
            if case let .file(url) = body { try? FileManager.default.removeItem(at: url) }
        }

        let (data, response) = try await send(request, body: body, progress: progress)
        return try parseResponse(data: data, response: response)
    }

    static func parseResponse(data: Data, response: URLResponse) throws -> URL {
        let text = String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        if let http = response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
            throw CatboxError.httpStatus(http.statusCode, body: text)
        }
        guard text.hasPrefix("https://") || text.hasPrefix("http://"), let url = URL(string: text) else {
            throw CatboxError.unexpectedResponse(text)
        }
        return url
    }

    private enum UploadBody {
        case data(Data)
        case file(URL)
    }

    private static func send(
        _ request: URLRequest, body: UploadBody, progress: UploadProgressHandler?
    ) async throws -> (Data, URLResponse) {
        try await withCheckedThrowingContinuation { continuation in
            let delegate = UploadDelegate(progress: progress, continuation: continuation)
            // A session per upload keeps delegate state simple; it is invalidated on completion.
            let session = URLSession(configuration: .default, delegate: delegate, delegateQueue: nil)
            let task: URLSessionUploadTask
            switch body {
            case let .data(data): task = session.uploadTask(with: request, from: data)
            case let .file(url): task = session.uploadTask(with: request, fromFile: url)
            }
            task.resume()
        }
    }
}

/// Delegate callbacks arrive on the session's serial delegate queue, so the mutable state is not raced.
private final class UploadDelegate: NSObject, URLSessionDataDelegate, @unchecked Sendable {
    private let progress: UploadProgressHandler?
    private var continuation: CheckedContinuation<(Data, URLResponse), Error>?
    private var received = Data()

    init(progress: UploadProgressHandler?, continuation: CheckedContinuation<(Data, URLResponse), Error>) {
        self.progress = progress
        self.continuation = continuation
    }

    func urlSession(
        _ session: URLSession, task: URLSessionTask, didSendBodyData bytesSent: Int64,
        totalBytesSent: Int64, totalBytesExpectedToSend: Int64
    ) {
        progress?(totalBytesSent, totalBytesExpectedToSend)
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        received.append(data)
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if let error {
            continuation?.resume(throwing: error)
        } else if let response = task.response {
            continuation?.resume(returning: (received, response))
        } else {
            continuation?.resume(throwing: URLError(.badServerResponse))
        }
        continuation = nil
        session.finishTasksAndInvalidate()
    }
}
