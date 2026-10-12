import Foundation

/// A multipart/form-data body. Files are streamed from disk when encoded to a file,
/// so large uploads (litterbox allows up to 1 GB) never have to fit in memory.
struct MultipartForm {
    enum Part {
        case field(name: String, value: String)
        case data(name: String, filename: String, contentType: String, Data)
        case file(name: String, filename: String, contentType: String, URL)
    }

    let boundary: String
    private(set) var parts: [Part] = []

    init(boundary: String = "catbox-\(UUID().uuidString)") {
        self.boundary = boundary
    }

    var contentType: String { "multipart/form-data; boundary=\(boundary)" }

    var containsFiles: Bool {
        parts.contains { if case .file = $0 { return true } else { return false } }
    }

    mutating func append(_ part: Part) {
        parts.append(part)
    }

    func encodedData() throws -> Data {
        var data = Data()
        try encode { data.append($0) }
        return data
    }

    func write(to url: URL) throws {
        guard FileManager.default.createFile(atPath: url.path, contents: nil) else {
            throw CocoaError(.fileWriteUnknown, userInfo: [NSFilePathErrorKey: url.path])
        }
        let handle = try FileHandle(forWritingTo: url)
        defer { try? handle.close() }
        try encode { try handle.write(contentsOf: $0) }
    }

    private func encode(into sink: (Data) throws -> Void) throws {
        func emit(_ string: String) throws { try sink(Data(string.utf8)) }

        for part in parts {
            try emit("--\(boundary)\r\n")
            switch part {
            case let .field(name, value):
                try emit("Content-Disposition: form-data; name=\"\(Self.escape(name))\"\r\n\r\n")
                try emit(value)
            case let .data(name, filename, contentType, data):
                try emit(Self.fileHeader(name: name, filename: filename, contentType: contentType))
                try sink(data)
            case let .file(name, filename, contentType, url):
                try emit(Self.fileHeader(name: name, filename: filename, contentType: contentType))
                let handle = try FileHandle(forReadingFrom: url)
                defer { try? handle.close() }
                while let chunk = try handle.read(upToCount: 1 << 20), !chunk.isEmpty {
                    try sink(chunk)
                }
            }
            try emit("\r\n")
        }
        try emit("--\(boundary)--\r\n")
    }

    private static func fileHeader(name: String, filename: String, contentType: String) -> String {
        "Content-Disposition: form-data; name=\"\(escape(name))\"; filename=\"\(escape(filename))\"\r\n"
            + "Content-Type: \(contentType)\r\n\r\n"
    }

    /// Quotes and line breaks would break out of the header, so escape or drop them.
    static func escape(_ value: String) -> String {
        value
            .replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "\"", with: "\\\"")
            .replacingOccurrences(of: "\r", with: "")
            .replacingOccurrences(of: "\n", with: "")
    }
}
