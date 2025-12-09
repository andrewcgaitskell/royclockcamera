/*
  ESP32-CAM (AI-Thinker) hourly capture + SD HTTP server
  + WS2811 (NeoPixel) support:
    - turn LEDs ON 2 seconds before capture
    - turn LEDs OFF 2 seconds after capture

  Board: AI-Thinker ESP32-CAM
*/

#include "esp_camera.h"
#include "WiFi.h"
#include "FS.h"
#include "SD_MMC.h"
#include <time.h>

#include <Adafruit_NeoPixel.h> // WS2811/NeoPixel support

#include "sd_http_server.h" // web module

//
// --------- CONFIG ---------
const char* WIFI_SSID = "";            // set to your SSID, or leave empty to use AP mode
const char* WIFI_PASS = "";            // set WiFi password
const char* AP_SSID = "ESP32-CAM-SD";  // AP SSID if no STA credentials provided
const char* AP_PASS = "12345678";

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;         // set to your timezone offset in seconds
const int   daylightOffset_sec = 0;

// retention policy: set maxFilesToKeep to 0 to disable automatic deletion
const size_t maxFilesToKeep = 0; // set >0 to enable retention

unsigned long lastPhotoEpoch = 0;
int lastCaptureHour = -1;

//
// --------- WS2811 (NeoPixel) CONFIG ---------
// Change these to match your LED wiring and count.
// If you don't want LEDs, set LED_COUNT to 0.
#define LED_PIN         4      // GPIO used to drive WS2811 / NeoPixel data
#define LED_COUNT       8      // number of LEDs in the strip (set 0 to disable)
#define LED_BRIGHTNESS  50     // 0-255

static Adafruit_NeoPixel strip(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

//
// --------- CAMERA PINS for AI-THINKER ---------
// Standard AI-Thinker pin mapping
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

// ------------------ helpers ------------------
String timeStampFilename(struct tm &tm) {
  char buf[64];
  // img_YYYYMMDD_HHMMSS.jpg
  sprintf(buf, "/img_%04d%02d%02d_%02d%02d%02d.jpg",
          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
          tm.tm_hour, tm.tm_min, tm.tm_sec);
  return String(buf);
}

// WS2811 helpers: init, on, off
void ws_init() {
  if (LED_COUNT == 0) return;
  strip.begin();
  strip.setBrightness(LED_BRIGHTNESS);
  strip.show(); // clear
}

void ws_on(uint8_t r = 255, uint8_t g = 255, uint8_t b = 255) {
  if (LED_COUNT == 0) return;
  for (uint16_t i = 0; i < strip.numPixels(); ++i) {
    strip.setPixelColor(i, strip.Color(r, g, b));
  }
  strip.show();
}

void ws_off() {
  if (LED_COUNT == 0) return;
  strip.clear();
  strip.show();
}

// capture photo and save to SD card, returns path or empty string on error
// This function now turns WS2811 LEDs on 2s before capture and off 2s after.
String captureAndSave() {
  const int maxAttempts = 3;
  camera_fb_t * fb = nullptr;
  int attempt = 0;

  // Turn on WS2811 lights 2 seconds before capture
  if (LED_COUNT > 0) {
    Serial.println("Turning LEDs ON (pre-capture)...");
    ws_on();           // default white, you can change color by passing RGB
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
    // sometimes fb->len can be 0 on a bad capture
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
    // ensure LEDs get turned off on failure
    if (LED_COUNT > 0) {
      Serial.println("Turning LEDs OFF (capture failed)");
      ws_off();
    }
    return String();
  }

  time_t now;
  time(&now);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo); // use UTC; use localtime_r if you set timezone differently

  String path = timeStampFilename(timeinfo);
  Serial.printf("Saving image to: %s  size=%u\n", path.c_str(), fb->len);

  File file = SD_MMC.open(path, FILE_WRITE);
  if(!file){
    Serial.println("Failed to open file for writing");
    // make sure to return fb to avoid leaking the buffer
    esp_camera_fb_return(fb);
    // ensure LEDs get turned off
    if (LED_COUNT > 0) {
      Serial.println("Turning LEDs OFF (file write failed)");
      ws_off();
    }
    return String();
  }

  size_t written = file.write(fb->buf, fb->len);
  file.close();

  // return framebuffer to driver so it can be reused / freed
  esp_camera_fb_return(fb);

  if (written != fb->len) {
    Serial.printf("Warning: wrote %u of %u bytes to %s\n", (unsigned)written, (unsigned)fb->len, path.c_str());
    // ensure LEDs get turned off
    if (LED_COUNT > 0) {
      Serial.println("Turning LEDs OFF (incomplete write)");
      ws_off();
    }
    // Optionally remove incomplete file
    // SD_MMC.remove(path);
    return String();
  }

  // keep lights on for 2 seconds after capture (post-capture)
  if (LED_COUNT > 0) {
    delay(2000);
    Serial.println("Turning LEDs OFF (post-capture)");
    ws_off();
  }

  // update last photo info
  lastPhotoEpoch = now;
  lastCaptureHour = timeinfo.tm_hour;

  // enforce retention policy in the web module (if enabled)
  sdws_enforceRetentionPolicy();

  return path;
}

// ------------------ setup functions ------------------
void initCamera() {
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
  // init with high resolution if you need large images; watch memory
  config.frame_size = FRAMESIZE_SVGA; // try FRAMESIZE_VGA or FRAMESIZE_SVGA
  config.jpeg_quality = 12; // 0-63 lower means higher quality

  // Use 2 frame buffers (double-buffered). This helps ensure fresh frames
  // are available when esp_camera_fb_get() is called.
  config.fb_count = 2;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    while(true) delay(1000);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("ESP32-CAM hourly capture + SD HTTP server starting...");

  // Initialize WS2811 (if configured)
  ws_init();

  // Initialize camera
  initCamera();

  // Mount SD card (SDMMC)
  if(!SD_MMC.begin()){
    Serial.println("SD_MMC mount failed. Check wiring/board. If your board uses SPI, switch to SD library.");
  } else {
    uint8_t cardType = SD_MMC.cardType();
    if(cardType == CARD_NONE){
      Serial.println("No SD Card attached");
    } else {
      Serial.println("SD Card mounted.");
    }
  }

  // Setup network: STA if credentials provided, otherwise AP
  if (strlen(WIFI_SSID) > 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("Connecting to WiFi SSID=%s ...\n", WIFI_SSID);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
      delay(500);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println();
      Serial.print("Connected, IP: ");
      Serial.println(WiFi.localIP());
    } else {
      Serial.println();
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

  // Start NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Waiting for time sync...");
  time_t now = time(nullptr);
  int tries = 0;
  while (now < 24 * 3600 && tries < 10) {
    delay(1000);
    Serial.print(".");
    now = time(nullptr);
    tries++;
  }
  Serial.println();
  if (now < 24 * 3600) {
    Serial.println("Failed to get time; photos will still be taken but filenames may be without correct timestamp");
  } else {
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    Serial.printf("Current time (UTC): %04d-%02d-%02d %02d:%02d:%02d\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  }

  // Configure and start the web server module
  sdws_setMaxFilesToKeep(maxFilesToKeep);
  sdws_begin();

  // initialize last capture hour so first capture happens on the next hour
  struct tm tmnow;
  time_t tt = time(nullptr);
  if (tt > 0) gmtime_r(&tt, &tmnow), lastCaptureHour = tmnow.tm_hour;
  else lastCaptureHour = -1;
}

void loop() {
  sdws_handleClient();

  time_t now = time(nullptr);
  if (now > 0) {
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);

    // Capture once per hour when hour changes
    if (timeinfo.tm_hour != lastCaptureHour) {
      // capture exactly at minute==0
      if (timeinfo.tm_min == 0) {
        Serial.printf("Hour changed to %02d -- taking photo\n", timeinfo.tm_hour);
        String saved = captureAndSave();
        if (saved.length()) {
          Serial.printf("Saved photo: %s\n", saved.c_str());
        } else {
          Serial.println("Failed to save photo.");
        }
      } else {
        // waiting until minute==0 for exact-on-the-hour capture
      }
    }
  }

  delay(1000);
}