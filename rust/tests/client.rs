//! Exercises the library against a one-shot HTTP server on localhost. No real network access.

use std::collections::HashMap;
use std::io::{BufRead, BufReader, Cursor, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::PathBuf;
use std::sync::mpsc;
use std::thread;

use catbox::{CatboxClient, Error, Expiry, LitterboxClient};

/// What the server received.
#[derive(Debug)]
struct Received {
    path: String,
    headers: HashMap<String, String>,
    fields: HashMap<String, String>,
    files: HashMap<String, FilePart>,
}

#[derive(Debug)]
struct FilePart {
    filename: String,
    content_type: String,
    data: Vec<u8>,
}

/// Starts a server that answers one request with `status` and `body`, and returns its URL plus
/// a channel that yields the parsed request.
fn serve_once(status: u16, body: &'static str) -> (String, mpsc::Receiver<Received>) {
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let url = format!("http://{}/user/api.php", listener.local_addr().unwrap());
    let (sender, receiver) = mpsc::channel();
    thread::spawn(move || {
        let (stream, _) = listener.accept().unwrap();
        let received = handle(stream, status, body);
        let _ = sender.send(received);
    });
    (url, receiver)
}

fn handle(stream: TcpStream, status: u16, body: &str) -> Received {
    let mut reader = BufReader::new(stream.try_clone().unwrap());
    let mut request_line = String::new();
    reader.read_line(&mut request_line).unwrap();
    let path = request_line.split(' ').nth(1).unwrap().to_owned();

    let mut headers = HashMap::new();
    loop {
        let mut line = String::new();
        reader.read_line(&mut line).unwrap();
        let line = line.trim_end();
        if line.is_empty() {
            break;
        }
        let (name, value) = line.split_once(':').unwrap();
        headers.insert(name.to_ascii_lowercase(), value.trim().to_owned());
    }
    assert!(
        !headers.contains_key("transfer-encoding"),
        "chunked bodies are not allowed"
    );
    let length: usize = headers["content-length"].parse().unwrap();
    let mut data = vec![0; length];
    reader.read_exact(&mut data).unwrap();

    let mut stream = stream;
    write!(
        stream,
        "HTTP/1.1 {status} X\r\nContent-Type: text/plain\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
        body.len()
    )
    .unwrap();

    let (fields, files) = parse_multipart(&headers["content-type"], &data);
    Received {
        path,
        headers,
        fields,
        files,
    }
}

fn parse_multipart(
    content_type: &str,
    body: &[u8],
) -> (HashMap<String, String>, HashMap<String, FilePart>) {
    let boundary = content_type.split("boundary=").nth(1).unwrap();
    let delimiter = format!("--{boundary}").into_bytes();
    let mut chunks = split(body, &delimiter);
    assert!(chunks.remove(0).is_empty());
    assert_eq!(chunks.pop().unwrap(), b"--\r\n");

    let (mut fields, mut files) = (HashMap::new(), HashMap::new());
    for chunk in chunks {
        let chunk = &chunk[2..chunk.len() - 2]; // strip the CRLFs around each part
        let split_at = chunk.windows(4).position(|w| w == b"\r\n\r\n").unwrap();
        let head = std::str::from_utf8(&chunk[..split_at]).unwrap();
        let content = chunk[split_at + 4..].to_vec();
        let attribute = |key: &str| {
            head.split(&format!("{key}=\""))
                .nth(1)
                .map(|rest| rest[..rest.find('"').unwrap()].to_owned())
        };
        let name = attribute(" name").unwrap();
        match attribute("filename") {
            Some(filename) => {
                let content_type = head
                    .lines()
                    .find_map(|line| line.strip_prefix("Content-Type: "))
                    .unwrap()
                    .to_owned();
                files.insert(
                    name,
                    FilePart {
                        filename,
                        content_type,
                        data: content,
                    },
                );
            }
            None => {
                fields.insert(name, String::from_utf8(content).unwrap());
            }
        }
    }
    (fields, files)
}

fn split<'a>(haystack: &'a [u8], needle: &[u8]) -> Vec<&'a [u8]> {
    let mut parts = Vec::new();
    let mut start = 0;
    let mut i = 0;
    while i + needle.len() <= haystack.len() {
        if &haystack[i..i + needle.len()] == needle {
            parts.push(&haystack[start..i]);
            i += needle.len();
            start = i;
        } else {
            i += 1;
        }
    }
    parts.push(&haystack[start..]);
    parts
}

/// A scratch directory that is deleted when dropped.
struct TempDir(PathBuf);

impl TempDir {
    fn new(name: &str) -> Self {
        let dir = std::env::temp_dir().join(format!("catbox-rust-{name}-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        TempDir(dir)
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.0);
    }
}

#[test]
fn uploads_a_file_from_disk() {
    let (endpoint, received) = serve_once(200, "https://files.catbox.moe/abc123.jpg\n");
    let data: Vec<u8> = (0..300_000u32).map(|i| (i % 251) as u8).collect();
    let dir = TempDir::new("upload");
    std::fs::create_dir_all(dir.0.join("nested")).unwrap();
    let path = dir.0.join("nested").join("photo.JPG");
    std::fs::write(&path, &data).unwrap();

    let client = CatboxClient::new()
        .with_user_hash("HASH123")
        .with_endpoint(endpoint);
    let url = client.upload_file(&path, None).unwrap();
    assert_eq!(url, "https://files.catbox.moe/abc123.jpg");

    let request = received.recv().unwrap();
    assert_eq!(request.path, "/user/api.php");
    assert_eq!(request.headers["user-agent"], "catbox-rust/0.1.0");
    assert_eq!(request.fields["reqtype"], "fileupload");
    assert_eq!(request.fields["userhash"], "HASH123");
    let file = &request.files["fileToUpload"];
    assert_eq!(file.filename, "photo.JPG");
    assert_eq!(file.content_type, "image/jpeg");
    assert_eq!(file.data, data);
}

#[test]
fn omits_an_empty_user_hash() {
    let (endpoint, received) = serve_once(200, "https://files.catbox.moe/x.txt");
    let client = CatboxClient::new()
        .with_user_hash("")
        .with_endpoint(endpoint);
    client.upload_bytes(b"hello", "notes.txt", None).unwrap();

    let request = received.recv().unwrap();
    assert!(!request.fields.contains_key("userhash"));
    let file = &request.files["fileToUpload"];
    assert_eq!(
        (file.filename.as_str(), file.content_type.as_str()),
        ("notes.txt", "text/plain")
    );
    assert_eq!(file.data, b"hello");
}

#[test]
fn uploads_from_a_reader() {
    let (endpoint, received) = serve_once(200, "https://files.catbox.moe/x.bin");
    let client = CatboxClient::new().with_endpoint(endpoint);
    let mut reader = Cursor::new(b"skip-me:payload".to_vec());
    reader.set_position(8);
    client.upload_reader(reader, "blob", None).unwrap();

    let file = &received.recv().unwrap().files["fileToUpload"];
    assert_eq!(file.data, b"payload");
    assert_eq!(file.content_type, "application/octet-stream");
}

#[test]
fn uploads_a_remote_url() {
    let (endpoint, received) = serve_once(200, "https://files.catbox.moe/copy.png");
    let client = CatboxClient::new()
        .with_user_hash("H")
        .with_endpoint(endpoint);
    let url = client.upload_url("https://example.com/image.png").unwrap();
    assert_eq!(url, "https://files.catbox.moe/copy.png");

    let request = received.recv().unwrap();
    assert_eq!(request.fields["reqtype"], "urlupload");
    assert_eq!(request.fields["url"], "https://example.com/image.png");
    assert_eq!(request.fields["userhash"], "H");
    assert!(request.files.is_empty());
}

#[test]
fn litterbox_sends_the_expiry() {
    let (endpoint, received) = serve_once(200, "https://litter.catbox.moe/t.zip");
    let client = LitterboxClient::new().with_endpoint(endpoint);
    let url = client
        .upload_bytes(b"PK", "t.zip", Expiry::TwelveHours, None)
        .unwrap();
    assert_eq!(url, "https://litter.catbox.moe/t.zip");

    let request = received.recv().unwrap();
    assert_eq!(request.fields["reqtype"], "fileupload");
    assert_eq!(request.fields["time"], "12h");
    assert!(!request.fields.contains_key("userhash"));
    assert_eq!(
        request.files["fileToUpload"].content_type,
        "application/zip"
    );
}

#[test]
fn reports_progress_up_to_the_body_size() {
    let (endpoint, received) = serve_once(200, "https://files.catbox.moe/p.bin");
    let data = vec![1u8; 2 * 1024 * 1024];
    let client = CatboxClient::new().with_endpoint(endpoint);
    let mut updates = Vec::new();
    client
        .upload_bytes(
            &data,
            "p.bin",
            Some(&mut |sent, total| updates.push((sent, total))),
        )
        .unwrap();

    let request = received.recv().unwrap();
    let length: u64 = request.headers["content-length"].parse().unwrap();
    assert!(updates.len() > 1);
    assert!(updates.windows(2).all(|pair| pair[0].0 < pair[1].0));
    assert!(updates.iter().all(|&(_, total)| total == length));
    assert_eq!(updates.last().unwrap().0, length);
}

#[test]
fn http_errors_carry_the_status() {
    let (endpoint, _received) = serve_once(500, "Internal Server Error");
    let client = CatboxClient::new().with_endpoint(endpoint);
    let error = client.upload_bytes(b"x", "x.txt", None).unwrap_err();
    assert_eq!(error.status(), Some(500));
    assert_eq!(
        error.to_string(),
        "server returned HTTP 500: Internal Server Error"
    );
}

#[test]
fn plain_text_answers_are_errors() {
    let (endpoint, _received) = serve_once(200, "No files given.");
    let client = CatboxClient::new().with_endpoint(endpoint);
    let error = client.upload_bytes(b"x", "x.txt", None).unwrap_err();
    assert!(matches!(&error, Error::UnexpectedResponse(body) if body == "No files given."));
    assert_eq!(error.status(), None);
}

#[test]
fn missing_files_fail_before_connecting() {
    // Nothing listens on this endpoint; a connection attempt would be a Network error instead.
    let client = CatboxClient::new().with_endpoint("http://127.0.0.1:9/");
    let error = client.upload_file("no/such/file.png", None).unwrap_err();
    assert!(
        matches!(&error, Error::Io(e) if e.kind() == std::io::ErrorKind::NotFound),
        "{error:?}"
    );
}

#[test]
fn refused_connections_are_network_errors() {
    let port = TcpListener::bind("127.0.0.1:0")
        .unwrap()
        .local_addr()
        .unwrap()
        .port();
    let client = CatboxClient::new().with_endpoint(format!("http://127.0.0.1:{port}/"));
    let error = client.upload_bytes(b"x", "x.txt", None).unwrap_err();
    assert!(matches!(error, Error::Network(_)), "{error:?}");
}
