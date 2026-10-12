import Foundation

public enum CatboxError: Error, Equatable {
    /// The server answered with a non-2xx status code.
    case httpStatus(Int, body: String)
    /// The server answered 2xx but the body was not a URL (catbox reports most errors this way).
    case unexpectedResponse(String)
}

extension CatboxError: LocalizedError {
    public var errorDescription: String? {
        switch self {
        case let .httpStatus(code, body):
            return body.isEmpty ? "server returned HTTP \(code)" : "server returned HTTP \(code): \(body)"
        case let .unexpectedResponse(body):
            return body.isEmpty ? "server returned an empty response" : "server error: \(body)"
        }
    }
}
