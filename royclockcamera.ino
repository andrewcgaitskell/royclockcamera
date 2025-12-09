/*
  ESP32-CAM SD card capture with accurate local time filenames
  + File listing & download endpoints (web) using esp_http_server

  Adapted from your working sketch. Adds:
  - /files  -> simple HTML listing of files on SD card (links to /download)
  - /download?file=<path-or-name> -> streams the requested file for download
  - /capture -> trigger an immediate dated capture and return download link

  Board: AI-Thinker ESP32-CAM (uses same camera config as your working sketch)
*/

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_timer.h"
#include "img_converters.h"
#include "Arduino.h"
#include "fb_gfx.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_http_server.h"

// Time
#include "time.h"

// MicroSD (ESP-IDF style)
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"

#include <ESPmDNS.h>
#include <dirent.h>

#include "secrets_34.h"
#include "secrets_roy.h"

// for fsync
#include <unistd.h>

#define PART_BOUNDARY "123456789000000000000987654321"
#define CAMERA_MODEL_AI_THINKER

// Camera Pin definition for AI Thinker module
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
camera_config_t config;

int file_number = 0;
bool internet_connected = false;
unsigned long lastNtpSync = 0;
int lastPhotoHour = -1;

// Simple flag to avoid stream/capture races (minimal approach)
volatile bool capturing = false;

// Track whether SD mount succeeded
bool sd_mounted = false;

// ---------- Streaming handler (simple check for capture in progress) ----------
static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t *_jpg_buf = NULL;
  char part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;

  while (true) {
    // If a capture is in progress, wait a bit so we don't race the capture code
    if (capturing) {
      delay(50);
      continue;
    }

    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      if (fb->width > 400) {
        if (fb->format != PIXFORMAT_JPEG) {
          bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
          esp_camera_fb_return(fb);
          fb = NULL; // Explicitly clear pointer
          if (!jpeg_converted) {
            Serial.println("JPEG compression failed");
            res = ESP_FAIL;
          }
        } else {
          _jpg_buf_len = fb->len;
          _jpg_buf = fb->buf;
        }
      }
    }
    if (res == ESP_OK) {
      size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL; // Always clear!
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if (res != ESP_OK) break;
    delay(1000);
  }
  return res;
}

// ---------- Camera server and other URIs ----------
void register_stream_endpoint(httpd_handle_t server);
static esp_err_t files_get_handler(httpd_req_t *req);
static esp_err_t download_get_handler(httpd_req_t *req);
static esp_err_t capture_get_handler(httpd_req_t *req);

void startCameraServer() {
  httpd_config_t config_http = HTTPD_DEFAULT_CONFIG();
  config_http.server_port = 80;

  // start server
  if (httpd_start(&stream_httpd, &config_http) == ESP_OK) {
    // streaming root
    httpd_uri_t index_uri = {
      .uri       = "/",
      .method    = HTTP_GET,
      .handler   = stream_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler(stream_httpd, &index_uri);

    // files listing
    httpd_uri_t files_uri = {
      .uri       = "/files",
      .method    = HTTP_GET,
      .handler   = files_get_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler(stream_httpd, &files_uri);

    // download
    httpd_uri_t download_uri = {
      .uri       = "/download",
      .method    = HTTP_GET,
      .handler   = download_get_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler(stream_httpd, &download_uri);

    // capture now
    httpd_uri_t capture_uri = {
      .uri       = "/capture",
      .method    = HTTP_GET,
      .handler   = capture_get_handler,
      .user_ctx  = NULL
    };
    httpd_register_uri_handler(stream_httpd, &capture_uri);
  } else {
    Serial.println("Failed to start HTTP server");
  }
}

// ---------- WiFi, mDNS, time, SD init (mostly unchanged) ----------
bool init_wifi() {
  const char* ssids[2] = {WIFI_SSID_34, WIFI_SSID_79};
  const char* passwords[2] = {WIFI_PASSWORD_34, WIFI_PASSWORD_79};
  const int maxConnAttempts = 10;
  for (int i = 0; i < 2; i++) {
    int connAttempts = 0;
    Serial.println("\r\nConnecting to: " + String(ssids[i]));
    WiFi.begin(ssids[i], passwords[i]);
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
      if (connAttempts++ > maxConnAttempts) break;
    }
    if (WiFi.status() == WL_CONNECTED) return true;
    WiFi.disconnect();
  }
  return false;
}

bool init_mdns() {
  if (!MDNS.begin("royclockcam_1")) {
    Serial.println("Error setting up MDNS responder!");
    while (1) delay(1000);
    return false;
  }
  Serial.println("mDNS responder started");
  MDNS.addService("http", "tcp", 80);
  return true;
}

void setup_time() {
  // Set timezone for UK/London, with DST
  setenv("TZ", "GMT0BST,M3.5.0/01,M10.5.0/02", 1);
  tzset();

  // London December: GMT, UTC+0, no DST (handled in TZ)
  configTime(0, 0, "pool.ntp.org");

  // Wait for time to be set
  time_t now = 0;
  struct tm timeinfo = {0};
  int retry = 0;
  const int retry_count = 10;
  while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
    Serial.printf("Waiting for system time to be set... (%d/%d)\n", retry, retry_count);
    delay(2000);
    time(&now);
    localtime_r(&now, &timeinfo);
  }
  Serial.printf("Current time: %s", asctime(&timeinfo));
}

static esp_err_t init_sdcard() {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    .max_files = 5,
  };
  sdmmc_card_t *card;
  Serial.println("Mounting SD card...");
  esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
  if (ret == ESP_OK) {
    Serial.println("SD card mount successfully!");
    sd_mounted = true;
  } else {
    Serial.printf("Failed to mount SD card VFAT filesystem. Error: %s\n", esp_err_to_name(ret));
    sd_mounted = false;
  }
  return ret;
}

// ---------- Image saving helpers (minimal changes: flush camera fb then get a fresh one + fsync) ----------
String make_dated_filename() {
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);

  char strftime_buf[32];
  strftime(strftime_buf, sizeof(strftime_buf), "%Y%m%d_%H%M%S", &timeinfo);
  char filename[64];
  snprintf(filename, sizeof(filename), "/sdcard/capture_%s.jpg", strftime_buf);
  return String(filename);
}

String make_numbered_filename() {
  file_number++;
  char filename[32];
  snprintf(filename, sizeof(filename), "/sdcard/capture_%d.jpg", file_number);
  return String(filename);
}

// Helper: flush the camera's current framebuffer(s) by grabbing and returning one, short delay,
// then attempt to capture a fresh framebuffer. Retries to be robust.
static camera_fb_t* flush_and_get_new_fb(int retries = 5, int delay_ms = 60) {
  camera_fb_t *fb = NULL;

  // If there's a currently held framebuffer inside the driver, get & return it to "flush"
  fb = esp_camera_fb_get();
  if (fb) {
    esp_camera_fb_return(fb);
    fb = NULL;
    // small pause to let camera advance
    delay(delay_ms);
  }

  // Now try to get a fresh frame
  for (int i = 0; i < retries; ++i) {
    fb = esp_camera_fb_get();
    if (fb && fb->len > 0) return fb;
    if (fb) {
      esp_camera_fb_return(fb);
      fb = NULL;
    }
    delay(delay_ms);
  }
  return NULL;
}

// returns empty string on failure, or full path on success
String save_photo_numbered_str() {
  String filename = make_numbered_filename();
  Serial.print("Taking picture: ");
  Serial.println(filename);

  camera_fb_t *fb = flush_and_get_new_fb();
  if (!fb) {
    Serial.println("Camera capture failed (no fresh fb)");
    return String();
  }

  // write in binary mode ("wb") and ensure flush to SD
  FILE *file = fopen(filename.c_str(), "wb");
  if (file != NULL) {
    size_t written = fwrite(fb->buf, 1, fb->len, file);
    fflush(file);
    int fd = fileno(file);
    if (fd >= 0) fsync(fd);
    Serial.printf("File saved: %s (bytes: %u)\n", filename.c_str(), (unsigned)written);
    fclose(file);
  } else {
    Serial.println("Could not open file for writing");
    esp_camera_fb_return(fb);
    return String();
  }
  esp_camera_fb_return(fb);
  return filename;
}

String save_photo_dated_str() {
  String filename = make_dated_filename();
  Serial.print("Taking picture: ");
  Serial.println(filename);

  camera_fb_t *fb = flush_and_get_new_fb();
  if (!fb) {
    Serial.println("Camera capture failed (no fresh fb)");
    return String();
  }

  // write in binary mode ("wb") and ensure flush to SD
  FILE *file = fopen(filename.c_str(), "wb");
  if (file != NULL) {
    size_t written = fwrite(fb->buf, 1, fb->len, file);
    fflush(file);
    int fd = fileno(file);
    if (fd >= 0) fsync(fd);
    Serial.printf("File saved: %s (bytes: %u)\n", filename.c_str(), (unsigned)written);
    fclose(file);
  } else {
    Serial.println("Could not open file for writing");
    esp_camera_fb_return(fb);
    return String();
  }
  esp_camera_fb_return(fb);
  return filename;
}

void save_photo(bool time_known) {
  // Defensive: Ensure previous framebuffers are released by called functions
  if (time_known) {
    save_photo_dated_str();
  } else {
    save_photo_numbered_str();
  }
}

// ---------- Helpers for content type ----------
String getContentTypeForFilename(const String& path) {
  String p = path;
  p.toLowerCase();
  if (p.endsWith(".htm") || p.endsWith(".html")) return "text/html";
  if (p.endsWith(".css")) return "text/css";
  if (p.endsWith(".js")) return "application/javascript";
  if (p.endsWith(".png")) return "image/png";
  if (p.endsWith(".jpg") || p.endsWith(".jpeg")) return "image/jpeg";
  if (p.endsWith(".gif")) return "image/gif";
  if (p.endsWith(".txt")) return "text/plain";
  return "application/octet-stream";
}

// ---------- /files handler: list files on SD (simple HTML) ----------
static esp_err_t files_get_handler(httpd_req_t *req) {
  Serial.println("/files handler called");
  if (!sd_mounted) {
    const char* msg = "SD card not mounted. /files unavailable.\n";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, msg, strlen(msg));
    return ESP_OK;
  }

  // Send a basic HTML page in chunks (avoid building one big string)
  const char* header = "<!doctype html><html><head><meta charset='utf-8'><title>ESP32-CAM SD Files</title></head><body><h2>Files on SD card</h2>";
  httpd_resp_send_chunk(req, header, strlen(header));

  DIR *dir = opendir("/sdcard");
  if (!dir) {
    const char* no = "<p>Unable to open /sdcard. Is the card mounted?</p></body></html>";
    httpd_resp_send_chunk(req, no, strlen(no));
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
  }

  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if (ent->d_type == DT_DIR) {
      // skip directories for now
      continue;
    }
    String name = String(ent->d_name);
    String line = "<a href=\"/download?file=" + name + "\">" + name + "</a><br>\n";
    httpd_resp_send_chunk(req, line.c_str(), line.length());
  }
  closedir(dir);

  const char* footer = "<hr><small>Use /download?file=FILENAME to download. Use /capture to take a photo now.</small></body></html>";
  httpd_resp_send_chunk(req, footer, strlen(footer));
  httpd_resp_send_chunk(req, NULL, 0); // end of response
  return ESP_OK;
}

// ---------- /download handler: stream file for download ----------
static esp_err_t download_get_handler(httpd_req_t *req) {
  char buf[512];
  // Get query string
  char query[256];
  int ret = httpd_req_get_url_query_str(req, query, sizeof(query));
  if (ret != ESP_OK) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  char file_param[224];
  if (httpd_query_key_value(query, "file", file_param, sizeof(file_param)) != ESP_OK) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  String requested = String(file_param);
  // Normalize requested path: if user provided basename, try /sdcard/<name>
  String path;
  if (requested.startsWith("/sdcard/") || requested.startsWith("/")) {
    path = requested;
  } else {
    path = "/sdcard/" + requested;
  }

  // Try to open file
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) {
    // as a fallback, try leading slash removal and also try /sdcard/<basename>
    // already tried /sdcard/<basename> above. If failed, respond 404
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  // Determine filename for Content-Disposition
  String filename = requested;
  int pos = filename.lastIndexOf('/');
  if (pos >= 0) filename = filename.substring(pos + 1);

  String ctype = getContentTypeForFilename(filename);
  httpd_resp_set_type(req, ctype.c_str());

  // Prevent browser caching of download results
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");

  // Content-Disposition header to force download
  String disp = "attachment; filename=\"" + filename + "\"";
  httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());

  // Stream file in chunks
  const size_t chunk_size = 1024;
  static uint8_t chunk[chunk_size];
  size_t r;
  while ((r = fread(chunk, 1, chunk_size, f)) > 0) {
    if (httpd_resp_send_chunk(req, (const char*)chunk, r) != ESP_OK) {
      fclose(f);
      return ESP_FAIL;
    }
  }
  fclose(f);

  // signal end of chunks
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

// ---------- /capture handler: take a dated photo and return link ----------
static esp_err_t capture_get_handler(httpd_req_t *req) {
  Serial.println("/capture handler called");

  // Indicate capture in progress so streaming won't race with camera
  capturing = true;
  String saved = save_photo_dated_str(); // returns full path or empty
  capturing = false;

  Serial.print("capture result: ");
  Serial.println(saved);

  if (saved.length()) {
    // produce a simple text response with link
    String rel = saved;
    // make a relative param for /download (strip leading /sdcard/ if present)
    if (rel.startsWith("/sdcard/")) rel = rel.substring(strlen("/sdcard/"));
    if (rel.startsWith("/")) rel = rel.substring(1);
    String resp = "Saved: " + saved + "\nDownload URL: /download?file=" + rel + "\n";
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp.c_str(), resp.length());
    return ESP_OK;
  } else {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Capture failed. Check Serial output.\n", strlen("Capture failed. Check Serial output.\n"));
    return ESP_FAIL;
  }
}

// ---------- Main control ----------
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  Serial.setDebugOutput(false);

  // Camera pin setup
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 15;
    config.fb_count = 1;
  }
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (init_wifi()) {
    internet_connected = true;
    Serial.println("Internet connected");
    setup_time();
  } else {
    internet_connected = false;
    Serial.println("WiFi failed");
  }

  init_mdns();

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    return;
  }
  esp_err_t sd_err = init_sdcard();
  if (sd_err != ESP_OK) {
    Serial.printf("SD Card init failed with error 0x%x\n", sd_err);
    // still continue so streaming may work (but /files and /download will fail)
  }

  Serial.print("Camera Stream Ready! Go to: http://");
  Serial.println(WiFi.localIP());

  startCameraServer();
}

void loop() {
  // Sync time from NTP every hour if connected
  if (internet_connected && millis() - lastNtpSync > 3600000UL) {
    setup_time();
    lastNtpSync = millis();
  }

  // Check time and capture photo on the hour
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);

  bool time_known = (timeinfo.tm_year >= (2016 - 1900)) && internet_connected;

  if (time_known && timeinfo.tm_min == 0 && timeinfo.tm_sec == 0 && timeinfo.tm_hour != lastPhotoHour) {
    Serial.printf("Camera taking photo at %02d:00:00\n", timeinfo.tm_hour);
    save_photo(true);
    lastPhotoHour = timeinfo.tm_hour;
    delay(2000);
  } else if (!time_known && lastPhotoHour != -1) {
    // Fall back: save numbered if time unknown
    save_photo(false);
    lastPhotoHour = -1;
    delay(2000);
  } else {
    delay(200);
  }
}

