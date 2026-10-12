import Foundation

/// UniformTypeIdentifiers is Apple-only, so keep a small table that works everywhere.
enum MIMEType {
    static let fallback = "application/octet-stream"

    private static let byExtension: [String: String] = [
        "7z": "application/x-7z-compressed",
        "aac": "audio/aac",
        "avif": "image/avif",
        "bmp": "image/bmp",
        "css": "text/css",
        "csv": "text/csv",
        "flac": "audio/flac",
        "gif": "image/gif",
        "gz": "application/gzip",
        "heic": "image/heic",
        "htm": "text/html",
        "html": "text/html",
        "ico": "image/vnd.microsoft.icon",
        "jpeg": "image/jpeg",
        "jpg": "image/jpeg",
        "js": "text/javascript",
        "json": "application/json",
        "m4a": "audio/mp4",
        "md": "text/markdown",
        "mkv": "video/x-matroska",
        "mov": "video/quicktime",
        "mp3": "audio/mpeg",
        "mp4": "video/mp4",
        "ogg": "audio/ogg",
        "opus": "audio/opus",
        "pdf": "application/pdf",
        "png": "image/png",
        "svg": "image/svg+xml",
        "tar": "application/x-tar",
        "tif": "image/tiff",
        "tiff": "image/tiff",
        "txt": "text/plain",
        "wav": "audio/wav",
        "webm": "video/webm",
        "webp": "image/webp",
        "xml": "application/xml",
        "zip": "application/zip",
    ]

    static func forFilename(_ filename: String) -> String {
        let ext = (filename as NSString).pathExtension.lowercased()
        return byExtension[ext] ?? fallback
    }
}
