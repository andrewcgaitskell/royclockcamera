/*
  ESP32-CAM (AI-Thinker) hourly capture + SD HTTP server
  - Improved SD mount handling (tries root mount then /sdcard fallback)
  - Ensures files are written into active mountpoint (sdRoot)
  - flush() + close() + small delay after write to reduce chance of lost writes
  - Avoids writing if SD not mounted
  - Reduced default LED brightness; see hardware note in message

  IMPORTANT: If you're powering a WS2811 strip, use a proper 5V supply with
  sufficient current and a common ground. Brownouts during writes will
  cause resets and lost files.
*/

#include "esp_camera.h"
#include "WiFi.h"
#include "FS.h"
#include "SD_MMC.h"
#include <time.h>

#include <Adafruit_NeoPixel.h> // WS2811/NeoPixel support

#include "sd_http_server.h" // web module
#include "secrets.h" // ssid and pw

// --------- CONFIG ---------
const char* WIFI_SSID = WIFI_SSID_34;
const char* WIFI_PASS = WIFI_PASSWORD_34;

const char* AP_SSID = "ESP32-CAM-SD";  // AP SSID if no STA credentials provided
const char* AP_PASS = "12345678";

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;         // set to your timezone offset in seconds
const int   daylightOffset_sec = 0;

const size_t maxFilesToKeep = 0; // retention policy: 0 = disabled

unsigned long lastPhotoEpoch = 0;
int lastCaptureHour = -1;

//
// --------- WS2811 (NeoPixel) CONFIG ---------
// Change these to match your LED wiring and count.
// If you don't want LEDs, set LED_COUNT to 0.
#define LED_PIN         4      // GPIO used to drive WS2811 / NeoPixel data
#define LED_COUNT       8      // number of LEDs in the strip (set 0 to disable)
#define LED_BRIGHTNESS  30     // reduced default brightness to lower current draw (0-255)

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

//
// --------- CAMERA PINS for AI-THINKER ---------
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

// ------------------ SD mount handling ------------------
// sdRoot will be either "" (if SD mounted at "/") or "/sdcard" (if mounted at that mountpoint)
static String sdRoot = "";
static bool sdMounted = false;

void listSdRoot() {
  // open the correct root depending on mountpoint
  String rootPath = sdRoot.length() ? sdRoot : String("/");
  File root = SD_MMC.open(rootPath.c_str());
  if (!root) {
    Serial.printf("listSdRoot: unable to open %s after mount.\n", rootPath.c_str());
    return;
  }
  Serial.printf("Root directory listing for %s:\n", rootPath.c_str());
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

// Try mounting SD. Returns true on success and sets sdRoot appropriately.
bool tryMountSDVariants() {
  // First try mount without specifying mountpoint (root "/")
  Serial.println("Attempting SD_MMC.begin() (root mount)...");
  if (SD_MMC.begin()) {
    uint8_t ctype = SD_MMC.cardType();
    if (ctype != CARD_NONE) {
      sdMounted = true;
      sdRoot = ""; // root is SD
      Serial.println("SD_MMC mounted at root (/).");
      listSdRoot();
      return true;
    } else {
      SD_MMC.end();
      Serial.println("SD_MMC.begin() returned CARD_NONE.");
    }
  } else {
    Serial.println("SD_MMC.begin() (root) failed.");
  }

  // Fallback: try mounting at /sdcard with 1-bit mode (works on many AI-Thinker boards)
  Serial.println("Attempting SD_MMC.begin(\"/sdcard\", true) (1-bit mount)...");
  if (SD_MMC.begin("/sdcard", true)) {
    uint8_t ctype = SD_MMC.cardType();
    if (ctype != CARD_NONE) {
      sdMounted = true;
      sdRoot = "/sdcard";
      Serial.println("SD_MMC mounted at /sdcard.");
      listSdRoot();
      return true;
    } else {
      SD_MMC.end();
      Serial.println("SD_MMC.begin(\"/sdcard\", true) returned CARD_NONE.");
    }
  } else {
    Serial.println("SD_MMC.begin(\"/sdcard\", true) failed.");
  }

  sdMounted = false;
  sdRoot = "";
  return false;
}

// ------------------ helpers ------------------
// produce filename WITHOUT leading slash
String timeStampFilenameNoSlash(struct tm &tm) {
  char buf[64];
  // img_YYYYMMDD_HHMMSS.jpg (no leading slash)
  sprintf(buf, "img_%04d%02d%02d_%02d%02d%02d.jpg",
          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
          tm.tm_hour, tm.tm_min, tm.tm_sec);
  return String(buf);
}

// forward extern for web module /snap
extern String captureAndSave();

// capture photo and save to SD card, returns saved full path or empty string on error
String captureAndSave() {
  if (!sdMounted) {
    Serial.println("SD not mounted - capture will not save file. Capture aborted for safety.");
    return String(); // avoid attempting to write when no SD available
  }

  const int maxAttempts = 3;
  camera_fb_t * fb = nullptr;
  int attempt = 0;

  // Turn on WS2811 lights 2 seconds before capture
  if (LED_COUNT > 0) {
    Serial.println("Turning LEDs ON (pre-capture)...");
    ws_on();           // default white, change color by passing RGB
    delay(2000);       // wait 2s for lighting to stabilize
  }

  // Try to get a valid framebuffer, retry a few times
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
    break; // valid fb acquired
  }

  if(!fb){
    Serial.println("Camera capture failed after retries");
    if (LED_COUNT > 0) { Serial.println("Turning LEDs OFF (capture failed)"); ws_off(); }
    return String();
  }

  // build full path with mountpoint
  time_t now;
  time(&now);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);

  String name = timeStampFilenameNoSlash(timeinfo);
  String fullPath;
  if (sdRoot.length()) fullPath = sdRoot + "/" + name;
  else fullPath = "/" + name;

  Serial.printf("Saving image to: %s  size=%u\n", fullPath.c_str(), fb->len);

  File file = SD_MMC.open(fullPath.c_str(), FILE_WRITE);
  if(!file){
    Serial.println("Failed to open file for writing");
    esp_camera_fb_return(fb);
    if (LED_COUNT > 0) { Serial.println("Turning LEDs OFF (file open failed)"); ws_off(); }
    return String();
  }

  size_t written = file.write(fb->buf, fb->len);
  // try flush if available
  #if defined(FILE_WRITE) || defined(ARDUINO_ARCH_ESP32)
  // Many ESP32 File implementations support flush(); it's harmless if not present.
  file.flush();
  #endif
  file.close();

  // small pause to let VFS settle (helps with intermittent card issues)
  delay(150);

  // return framebuffer to driver so it can be reused / freed
  esp_camera_fb_return(fb);

  if (written != fb->len) {
    Serial.printf("Warning: wrote %u of %u bytes to %s\n", (unsigned)written, (unsigned)fb->len, fullPath.c_str());
    if (LED_COUNT > 0) { Serial.println("Turning LEDs OFF (incomplete write)"); ws_off(); }
    // Optionally remove incomplete file
    // SD_MMC.remove(fullPath.c_str());
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
  return fullPath;
}

// ------------------ camera init ------------------
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
  config.fb_count = fbCount; // use 1 to reduce PSRAM/pin conflicts

  Serial.printf("Initializing camera with fb_count=%d...\n", fbCount);
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    // continue so diagnostics can be gathered
  } else {
    Serial.println("Camera initialized.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("ESP32-CAM hourly capture + SD HTTP server - SD & power-hardening build");

  // init LEDs (if present)
  ws_init();

  // Try to mount SD (root or /sdcard fallback)
  sdMounted = tryMountSDVariants();
  if (!sdMounted) {
    Serial.println("WARNING: SD card not mounted. /snap will not write images until SD is mounted.");
  }

  // Init camera (use 1 fb to reduce conflicts)
  initCamera(1);

  // -- network --
  if (strlen(WIFI_SSID) > 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("Connecting to WiFi SSID=%s ...\n", WIFI_SSID);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.print("Connected, IP: ");
      Serial.println(WiFi.localIP());
      // NTP sync handled elsewhere / as before
    } else {
      Serial.println("Failed to connect, starting AP instead");
      WiFi.softAP(AP_SSID, AP_PASS);
      Serial.print("AP IP address: ");
      Serial.println(WiFi.softAPIP());
    }
  } else {
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
  } else lastCaptureHour = -1;
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
