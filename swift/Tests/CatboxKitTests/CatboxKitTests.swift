@testable import CatboxKit
import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import XCTest

final class MultipartFormTests: XCTestCase {
    func testEncodesFieldsAndData() throws {
        var form = MultipartForm(boundary: "B")
        form.append(.field(name: "reqtype", value: "fileupload"))
        form.append(.data(name: "fileToUpload", filename: "a.txt", contentType: "text/plain", Data("hi".utf8)))

        let expected = "--B\r\n"
            + "Content-Disposition: form-data; name=\"reqtype\"\r\n\r\n"
            + "fileupload\r\n"
            + "--B\r\n"
            + "Content-Disposition: form-data; name=\"fileToUpload\"; filename=\"a.txt\"\r\n"
            + "Content-Type: text/plain\r\n\r\n"
            + "hi\r\n"
            + "--B--\r\n"
        XCTAssertEqual(String(decoding: try form.encodedData(), as: UTF8.self), expected)
        XCTAssertEqual(form.contentType, "multipart/form-data; boundary=B")
        XCTAssertFalse(form.containsFiles)
    }

    func testStreamsFileContentsToDisk() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("catbox-tests-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }

        // Larger than one read chunk, to exercise the streaming loop.
        let payload = Data((0..<(3 << 20)).map { UInt8(truncatingIfNeeded: $0) })
        let source = dir.appendingPathComponent("big.bin")
        try payload.write(to: source)

        var fileForm = MultipartForm(boundary: "B")
        fileForm.append(.file(name: "f", filename: "big.bin", contentType: "application/octet-stream", source))
        var dataForm = MultipartForm(boundary: "B")
        dataForm.append(.data(name: "f", filename: "big.bin", contentType: "application/octet-stream", payload))

        let output = dir.appendingPathComponent("body")
        try fileForm.write(to: output)
        XCTAssertTrue(fileForm.containsFiles)
        XCTAssertEqual(try Data(contentsOf: output), try dataForm.encodedData())
    }

    func testEscapesHeaderValues() {
        XCTAssertEqual(MultipartForm.escape("a\"b\\c\r\nd"), "a\\\"b\\\\cd")
    }
}

final class ResponseTests: XCTestCase {
    private let endpoint = URL(string: "https://catbox.moe/user/api.php")!

    private func response(_ status: Int) -> HTTPURLResponse {
        HTTPURLResponse(url: endpoint, statusCode: status, httpVersion: nil, headerFields: nil)!
    }

    func testReturnsFullURL() throws {
        let url = try Transport.parseResponse(data: Data("https://files.catbox.moe/abc123.png\n".utf8), response: response(200))
        XCTAssertEqual(url.absoluteString, "https://files.catbox.moe/abc123.png")
    }

    func testErrorTextWithSuccessStatus() {
        XCTAssertThrowsError(try Transport.parseResponse(data: Data("No files given.".utf8), response: response(200))) {
            XCTAssertEqual($0 as? CatboxError, .unexpectedResponse("No files given."))
        }
    }

    func testHTTPErrorStatus() {
        XCTAssertThrowsError(try Transport.parseResponse(data: Data("too big".utf8), response: response(412))) {
            XCTAssertEqual($0 as? CatboxError, .httpStatus(412, body: "too big"))
        }
    }
}

final class MIMETypeTests: XCTestCase {
    func testLookup() {
        XCTAssertEqual(MIMEType.forFilename("photo.JPG"), "image/jpeg")
        XCTAssertEqual(MIMEType.forFilename("notes.txt"), "text/plain")
        XCTAssertEqual(MIMEType.forFilename("noextension"), "application/octet-stream")
        XCTAssertEqual(MIMEType.forFilename("archive.unknown"), "application/octet-stream")
    }
}
