# RoyClockCamera

ESP32-CAM sketch that:
- Streams MJPEG to `/` (browser).
- Lists files on the SD card at `/files`.
- Serves file downloads at `/download?file=<name>`.
- Triggers a dated capture at `/capture` and returns the download URL.

This repository contains a single sketch (royclockcamera.ino) that implements the camera, SD card storage and a small HTTP server using `esp_http_server`. The design keeps web server responsibilities and capture/write logic separated so there is no duplication of web-server code.

---

## Main responsibilities and functions

Summary of the important functions and what they do (refer to `royclockcamera.ino`):

- `startCameraServer()`
  - Central place that creates the HTTP server and registers all URI handlers.
  - This is the only place where handlers are registered, avoiding duplicated registration logic.

- HTTP handlers (each handler implements a single responsibility)
  - `stream_handler(httpd_req_t *req)`
    - Provides the MJPEG multipart stream on `/`.
    - Acquires the camera mutex, grabs a framebuffer, converts to JPEG if needed and streams it in chunks.
  - `files_get_handler(httpd_req_t *req)`
    - Lists files on the mounted SD card (`/sdcard`).
    - Streams a small HTML page in chunks; the page links to `/download?file=<filename>`.
  - `download_get_handler(httpd_req_t *req)`
    - Serves file contents for download (streams file in chunks).
    - Sets `Content-Disposition` and cache headers to avoid client-side caching of downloaded files.
  - `capture_get_handler(httpd_req_t *req)`
    - Triggers an immediate capture and writes a dated filename to the SD card.
    - Returns a small text response with a `/download?file=...` URL.

- Camera capture and save helpers
  - `save_photo(bool time_known)`
    - Public helper that chooses a dated filename (when time is known) or a numbered filename (fallback).
    - Uses a single safe implementation to capture and write image files.
  - `save_photo_dated_str()` / `save_photo_numbered_str()` (internal variants)
    - Perform the actual capture and file write logic.
    - All writes use `"wb"` (binary) mode and call `fflush()` + `fsync()` to ensure data reaches the SD card.
  - `make_dated_filename()`, `make_numbered_filename()`
    - Helpers to produce filenames for saved captures.

- Camera synchronization & freshness
  - A single FreeRTOS mutex `cameraLock` serializes camera access (streaming vs capture) to avoid races.
  - `flush_and_get_new_fb()` (or similar helper)
    - Attempts to flush a held framebuffer and obtain a fresh frame (with retries/delay).
    - Optionally checks a small sample checksum to detect and avoid saving the previous frame.

- Initialization helpers
  - `init_wifi()` — connects to configured WiFi networks (tries fallbacks).
  - `init_mdns()` — starts mDNS responder for local discovery.
  - `setup_time()` — configures timezone and NTP sync.
  - `init_sdcard()` — mounts the SD card using the ESP-IDF FAT VFS wrapper.

---

## Why there is no duplication of web server functionality

- All URI handlers are registered in exactly one place: `startCameraServer()`. Handlers are function pointers registered against a URI and HTTP method. There are no duplicate registrations nor repeated copies of handler code.

- The web server handlers do not perform file write logic; they call (or rely on) dedicated helper functions:
  - `/capture` calls the capture/save helper to perform camera access and file writes — it does not duplicate camera logic.
  - The hourly capture (in `loop`) also uses the same helper (`save_photo` or the internal capture function) — the capture/write logic is implemented once and reused.

- The download logic (serving files) is implemented once in `download_get_handler`. The `/files` listing simply creates links to the download endpoint — it does not reimplement streaming or file transfer logic.

- The MJPEG stream implementation is a distinct handler (`stream_handler`) focused solely on live stream behavior. It uses the camera and returns the frame directly, but it does not duplicate the file-write code.

In short:
- Server registration: one place (no duplicate registrations).
- Capture/file-write logic: single implementation, reused by the `/capture` endpoint and scheduled/hourly captures.
- File listing and download: separate single implementations; listing produces links to the single download endpoint.

---

## Key code snippets

Handler registration (single place; see `startCameraServer()`):
````markdown
```c++
httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = stream_handler };
httpd_register_uri_handler(stream_httpd, &index_uri);

httpd_uri_t files_uri = { .uri = "/files", .method = HTTP_GET, .handler = files_get_handler };
httpd_register_uri_handler(stream_httpd, &files_uri);

httpd_uri_t download_uri = { .uri = "/download", .method = HTTP_GET, .handler = download_get_handler };
httpd_register_uri_handler(stream_httpd, &download_uri);

httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_get_handler };
httpd_register_uri_handler(stream_httpd, &capture_uri);
```
