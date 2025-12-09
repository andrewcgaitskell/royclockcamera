/*
  ESP32-CAM (AI-Thinker) hourly capture + SD HTTP server
  Improved, robust NTP sync logic.

  What changed (NTP):
  - Ensures WiFi is connected before attempting NTP.
  - Calls configTime() with three servers for redundancy.
  - Waits up to a configurable timeout and verifies the returned year
    (tm_year + 1900) to confirm a real time was received.
  - Retries the sync sequence a few times with short backoff if it fails.
  - If running as AP (no STA credentials) or WiFi not connected, NTP is skipped.
  - Logs detailed diagnostics to Serial so you can paste the log if still failing.

  Keep the rest of your code (sd_http_server.*) as before; only the time sync portion
  and where it is called in setup() were changed.
*/

#include "esp_camera.h"
#include "WiFi.h"
#include "FS.h"
#include "SD_MMC.h"
#include <time.h>

#include <Adafruit_NeoPixel.h> // WS2811 if used
#include "sd_http_server.h"     // web module

#include "secrets.h"#include "secrets_34.h"


//
// --------- CONFIG ---------
const char* WIFI_SSID = WIFI_SSID_34;            // set to your SSID, or leave empty to use AP mode
const char* WIFI_PASS = WIFI_PASSWORD_34;            // set WiFi password

const char* AP_SSID = "ESP32-CAM-SD";  // AP SSID if no STA credentials provided
const char* AP_PASS = "12345678";

// Timezone / NTP
// set your timezone offset from UTC in seconds (e.g., UTC+1 => 3600)
const long  gmtOffset_sec = 0;
const int   daylightOffset_sec = 0;
// NTP servers - three for redundancy
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";
const char* ntpServer3 = "time.google.com";

const size_t maxFilesToKeep = 0; // retention policy

// WS2811 (NeoPixel) config (0 to disable)
#define LED_PIN         4
#define LED_COUNT       8
#define LED_BRIGHTNESS  50
static Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

void ws_init() {
  if (LED_COUNT == 0) return;
  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.show();
}
void ws_on(uint8_t r = 255, uint8_t g = 255, uint8_t b = 255) {
  if (LED_COUNT == 0) return;
  for (uint16_t i = 0; i < strip.numPixels(); ++i) strip.setPixelColor(i, strip.Color(r,g,b));
  strip.show();
}
void ws_off() {
  if (LED_COUNT == 0) return;
  strip.clear(); strip.show();
}

unsigned long lastPhotoEpoch = 0;
int lastCaptureHour = -1;

// forward extern used by web module
extern String captureAndSave();

// ---------------- NTP helper ----------------
// Returns true if time was synced (and logs the time), false otherwise.
// timeoutSec: how long to wait for a sync on each attempt.
// attempts: how many times to attempt the sync procedure (re-calls configTime each attempt).
bool syncTimeWithRetries(int attempts = 3, int timeoutSec = 30) {
  if (strlen(WIFI_SSID) == 0) {
    Serial.println("No STA credentials provided; running as AP. Skipping NTP sync.");
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected; cannot perform NTP sync.");
    return false;
  }

  for (int a = 1; a <= attempts; ++a) {
    Serial.printf("NTP sync attempt %d/%d: calling configTime(...)\n", a, attempts);
    // Configure SNTP/NTP servers. Use three servers for redundancy.
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2, ntpServer3);

    Serial.printf("Waiting up to %d seconds for time to be set by NTP...\n", timeoutSec);
    unsigned long start = millis();
    bool ok = false;
    while ((millis() - start) < (unsigned long)timeoutSec * 1000UL) {
      time_t now = time(nullptr);
      struct tm timeinfo;
      gmtime_r(&now, &timeinfo);
      // Consider time valid if year > 2016 (arbitrary reasonably-past year)
      if ((timeinfo.tm_year + 1900) > 2016) {
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &timeinfo);
        Serial.printf("NTP time synced: %s\n", buf);
        ok = true;
        break;
      }
      delay(1000);
    }

    if (ok) return true;

    Serial.printf("NTP attempt %d failed. ", a);
    if (a < attempts) {
      Serial.printf("Retrying after short delay...\n");
      delay(2000 * a); // backoff
    } else {
      Serial.println("No more retries.");
    }
  }

  Serial.println("Failed to sync time via NTP.");
  return false;
}

// ------------------ rest of sketch (camera, sd, capture) ------------------
// Camera pin definitions for AI-Thinker - unchanged from prior sketches
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

String timeStampFilename(struct tm &tm) {
  char buf[64];
  sprintf(buf, "/img_%04d%02d%02d_%02d%02d%02d.jpg",
          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
          tm.tm_hour, tm.tm_min, tm.tm_sec);
  return String(buf);
}

void listSdRoot() {
  File root = SD_MMC.open("/");
  if (!root) {
    Serial.println("listSdRoot: unable to open / after mount.");
    return;
  }
  Serial.println("Root directory listing:");
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    Serial.printf("  %s  %s  %u bytes\n",
                  entry.isDirectory() ? "DIR " : "FILE",
                  entry.name(), entry.size());
    entry.close();
  }
  root.close();
}

bool tryMountSD(bool oneBitMode = true) {
  Serial.printf("Attempting SD_MMC.begin(mountpoint=\"/sdcard\", 1bit=%s)...\n", oneBitMode ? "true" : "false");
  bool ok = SD_MMC.begin("/sdcard", oneBitMode);
  if (!ok) {
    Serial.println("SD_MMC.begin() failed.");
    return false;
  }
  uint8_t ctype = SD_MMC.cardType();
  if (ctype == CARD_NONE) {
    Serial.println("SD_MMC reports CARD_NONE after begin()");
    SD_MMC.end();
    return false;
  }
  Serial.print("SD_MMC mounted. Card type: ");
  if (ctype == CARD_MMC) Serial.println("MMC");
  else if (ctype == CARD_SD) Serial.println("SDSC");
  else if (ctype == CARD_SDHC) Serial.println("SDHC/SDXC");
  else Serial.println("UNKNOWN");
  listSdRoot();
  return true;
}

// captureAndSave() from earlier, unchanged except it uses time() after sync attempt
String captureAndSave() {
  const int maxAttempts = 3;
  camera_fb_t * fb = nullptr;
  int attempt = 0;

  // Turn on WS2811 lights 2 seconds before capture
  if (LED_COUNT > 0) {
    Serial.println("Turning LEDs ON (pre-capture)...");
    ws_on();
    delay(2000);
  }

  for(attempt = 0; attempt < maxAttempts; ++attempt) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.printf("Attempt %d: Camera capture returned NULL, retrying...\n", attempt+1);
      delay(200);
      continue;
    }
    if (fb->len == 0) {
      Serial.printf("Attempt %d: Camera capture length 0, returning fb and retrying...\n", attempt+1);
      esp_camera_fb_return(fb);
      fb = nullptr;
      delay(200);
      continue;
    }
    break;
  }

  if(!fb){
    Serial.println("Camera capture failed after retries");
    if (LED_COUNT > 0) { Serial.println("Turning LEDs OFF (capture failed)"); ws_off(); }
    return String();
  }

  time_t now;
  time(&now);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);

  String path = timeStampFilename(timeinfo);
  Serial.printf("Saving image to: %s  size=%u\n", path.c_str(), fb->len);

  File file = SD_MMC.open(path, FILE_WRITE);
  if(!file){
    Serial.println("Failed to open file for writing");
    esp_camera_fb_return(fb);
    if (LED_COUNT > 0) { Serial.println("Turning LEDs OFF (file write failed)"); ws_off(); }
    return String();
  }

  size_t written = file.write(fb->buf, fb->len);
  file.close();

  esp_camera_fb_return(fb);

  if (written != fb->len) {
    Serial.printf("Warning: wrote %u of %u bytes to %s\n", (unsigned)written, (unsigned)fb->len, path.c_str());
    if (LED_COUNT > 0) { Serial.println("Turning LEDs OFF (incomplete write)"); ws_off(); }
    return String();
  }

  // keep lights on for 2 seconds after capture (post-capture)
  if (LED_COUNT > 0) {
    delay(2000);
    Serial.println("Turning LEDs OFF (post-capture)");
    ws_off();
  }

  lastPhotoEpoch = now;
  lastCaptureHour = timeinfo.tm_hour;
  sdws_enforceRetentionPolicy();
  return path;
}

// camera init (reduced fb_count to 1 to reduce PSRAM/pin conflicts)
void initCamera(uint8_t fbCount = 1) {
  camera_config_t config;
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
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_SVGA;
  config.jpeg_quality = 12;
  config.fb_count = fbCount;

  Serial.printf("Initializing camera with fb_count=%d...\n", fbCount);
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    // allow program to continue so we can diagnose more
  } else {
    Serial.println("Camera initialized.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("ESP32-CAM hourly capture + SD HTTP server - NTP robustness build");

  ws_init();

  // Mount SD before/after camera as before (not shown again here)...
  bool sdMounted = tryMountSD(true);
  if (!sdMounted) {
    initCamera(1);
    Serial.println("Retrying SD_MMC.begin() after camera init...");
    sdMounted = tryMountSD(true);
  } else {
    initCamera(1);
  }
  if (!sdMounted) {
    Serial.println("WARNING: SD card not mounted. /snap will still attempt capture but images won't be saved.");
  }

  // Network setup (STA if WIFI_SSID set, otherwise AP)
  if (strlen(WIFI_SSID) > 0) {
    Serial.printf("Connecting to WiFi SSID=%s ...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
      delay(250);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("Connected, IP: ");
      Serial.println(WiFi.localIP());
      // Attempt to sync time now (robust routine)
      bool synced = syncTimeWithRetries(3, 25); // 3 attempts, 25s each
      if (!synced) {
        Serial.println("NTP sync did not succeed during setup. The device will continue to operate; you can retry sync from Serial or trigger /snap later.");
      }
    } else {
      Serial.println("Failed to connect to WiFi; starting AP instead");
      WiFi.softAP(AP_SSID, AP_PASS);
      Serial.print("AP IP address: ");
      Serial.println(WiFi.softAPIP());
    }
  } else {
    Serial.println("No STA credentials provided; starting AP mode and skipping NTP sync.");
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print("AP IP address: ");
    Serial.println(WiFi.softAPIP());
  }

  // start webserver
  sdws_setMaxFilesToKeep(maxFilesToKeep);
  sdws_begin();

  // init last capture hour
  time_t tt = time(nullptr);
  if (tt > 0) {
    struct tm tmnow;
    gmtime_r(&tt, &tmnow);
    lastCaptureHour = tmnow.tm_hour;
  } else {
    lastCaptureHour = -1;
  }
}

void loop() {
  sdws_handleClient();

  time_t now = time(nullptr);
  if (now > 0) {
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    if (timeinfo.tm_hour != lastCaptureHour) {
      if (timeinfo.tm_min == 0) {
        Serial.printf("Hour changed to %02d -- taking photo\n", timeinfo.tm_hour);
        String saved = captureAndSave();
        if (saved.length()) {
          Serial.printf("Saved photo: %s\n", saved.c_str());
        } else {
          Serial.println("Failed to save photo.");
        }
      }
    }
  }
  delay(1000);
}
