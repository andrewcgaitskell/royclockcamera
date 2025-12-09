#include "sd_http_server.h"
#include <WebServer.h>
#include <FS.h>
#include "SD_MMC.h"
#include <vector>
#include <algorithm>

// Note: this module does not assume a specific mountpoint. It tries to detect whether
// files live at "/" or "/sdcard" (common on ESP32 boards) and will list/download from
// whichever location contains image files. This makes the web UI work even if SD was
// mounted at root or at /sdcard.

static WebServer server(80);
static size_t s_maxFilesToKeep = 0;

// helper: get best mount root by trying "/" then "/sdcard" and returning the one
// that has files (prefers "/" if both have files).
static String detectSdRoot() {
  // quick helper to count non-directory files in a path
  auto countFiles = [](const char* path)->size_t {
    File root = SD_MMC.open(path);
    if (!root) return 0;
    size_t cnt = 0;
    while (true) {
      File entry = root.openNextFile();
      if (!entry) break;
      if (!entry.isDirectory()) ++cnt;
      entry.close();
    }
    root.close();
    return cnt;
  };

  // If SD isn't mounted, cardType() returns CARD_NONE
  if (SD_MMC.cardType() == CARD_NONE) return String();

  size_t cntRoot = countFiles("/");
  size_t cntSdcard = countFiles("/sdcard");

  if (cntRoot == 0 && cntSdcard == 0) {
    // fallback: if SD is mounted but empty, prefer sdcard (common)
    // detect whether /sdcard opens at all
    File trySd = SD_MMC.open("/sdcard");
    if (trySd) {
      trySd.close();
      return String("/sdcard");
    }
    File tryRoot = SD_MMC.open("/");
    if (tryRoot) {
      tryRoot.close();
      return String("/");
    }
    return String();
  }

  // prefer root if it has files, otherwise sdcard
  if (cntRoot >= cntSdcard) return String("/");
  return String("/sdcard");
}

// helper: produce content type
static String getContentType(const String& path){
  if(path.endsWith(".htm") || path.endsWith(".html")) return "text/html";
  if(path.endsWith(".css")) return "text/css";
  if(path.endsWith(".js")) return "application/javascript";
  if(path.endsWith(".png")) return "image/png";
  if(path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if(path.endsWith(".gif")) return "image/gif";
  if(path.endsWith(".txt")) return "text/plain";
  return "application/octet-stream";
}

// helper: list directory at provided path and append HTML listing into out
static void printDirectoryHtmlAt(File dir, String &out){
  while(true){
    File entry = dir.openNextFile();
    if(!entry) break;
    String name = String(entry.name());
    if(entry.isDirectory()){
      out += "<b>" + name + "/</b><br>";
      printDirectoryHtmlAt(entry, out);
    } else {
      // name may be full path or relative depending on mount; show relative name and create download link
      String rel = name;
      // streamFile and /download handling will try multiple prefixes
      out += "<a href=\"/download?file=" + rel + "\">" + rel + "</a> (" + String(entry.size()) + " bytes)<br>";
    }
    entry.close();
  }
}

static void handleRoot(){
  String html = "<!doctype html><html><head><meta charset='utf-8'><title>ESP32-CAM SD</title></head><body>";
  html += "<h2>Files on SD card</h2>";

  if (SD_MMC.cardType() == CARD_NONE) {
    html += "SD card not mounted.<br>";
  } else {
    String root = detectSdRoot();
    if (root.length() == 0) {
      html += "SD mounted but no files found (or unable to access mountpoint).<br>";
    } else {
      html += "<p>Listing for: " + root + "</p>";
      File dir = SD_MMC.open(root.c_str());
      if(!dir){
        html += "Failed to open directory at " + root + "<br>";
      } else {
        printDirectoryHtmlAt(dir, html);
        dir.close();
      }
    }
  }

  html += "<hr><small>Use /download?file=/img_YYYY... or /download?file=img_... to download or /snap to take a photo now</small></body></html>";
  server.send(200, "text/html", html);
}

// Attempt to open the requested filePath using a few candidate prefixes.
// Accepts incoming file param like "img_..." or "/img_..." or "/sdcard/img_..."
static File openFileWithPrefixes(const String& reqFile) {
  String f = reqFile;
  if (f.length() == 0) return File();

  // normalize: remove leading "./"
  while (f.startsWith("./")) f = f.substring(2);

  // if user passed with leading slash keep it
  bool startsSlash = f.startsWith("/");

  // Candidate list:
  // 1) as-provided
  // 2) if it doesn't start with / and /sdcard exists -> "/sdcard/" + f
  // 3) if it doesn't start with / -> "/" + f
  // 4) / + f (if not yet tried)
  // 5) try root guess from detectSdRoot + f
  File file;

  // 1) try as-provided
  file = SD_MMC.open(f.c_str());
  if (file) return file;

  // 2) try /sdcard/<f> (if not already)
  if (!f.startsWith("/")) {
    String candidate = String("/sdcard/") + f;
    file = SD_MMC.open(candidate.c_str());
    if (file) return file;
  }

  // 3) try /<f>
  if (!f.startsWith("/")) {
    String candidate = String("/") + f;
    file = SD_MMC.open(candidate.c_str());
    if (file) return file;
  }

  // 4) if user supplied absolute path but file open failed, try trimming leading slash and prefixing /sdcard/
  if (f.startsWith("/")) {
    String trim = f.substring(1);
    String candidate = String("/sdcard/") + trim;
    file = SD_MMC.open(candidate.c_str());
    if (file) return file;
    candidate = String("/") + trim;
    file = SD_MMC.open(candidate.c_str());
    if (file) return file;
  }

  // 5) try detectSdRoot + "/" + basename
  String root = detectSdRoot();
  if (root.length()) {
    String base = f;
    // if f includes path separators, keep them (user might pass path). Otherwise use base
    if (base.startsWith("/")) base = base.substring(1);
    String candidate = root + "/" + base;
    file = SD_MMC.open(candidate.c_str());
    if (file) return file;
  }

  // nothing found
  return File();
}

static void handleDownload(){
  if(!server.hasArg("file")){
    server.send(400, "text/plain", "Missing file parameter");
    return;
  }
  String filePath = server.arg("file");
  // try to open with multiple strategies
  File f = openFileWithPrefixes(filePath);
  if(!f || f.isDirectory()){
    server.send(404, "text/plain", "File not found");
    if(f) f.close();
    return;
  }
  String contentType = getContentType(filePath);
  // choose filename for Content-Disposition: use basename of path
  String filename = filePath;
  // strip any path
  int lastSlash = filename.lastIndexOf('/');
  if (lastSlash >= 0) filename = filename.substring(lastSlash + 1);
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server.streamFile(f, contentType);
  f.close();
}

// /snap handler: triggers captureAndSave() and returns result
// captureAndSave() is implemented in the main sketch and declared extern there.
extern String captureAndSave();
static void handleSnap(){
  Serial.println("HTTP /snap requested - triggering capture");
  String saved = captureAndSave();
  if (saved.length()) {
    String resp = "Saved: " + saved + "\n";
    // convert saved to a download link relative to webserver
    // saved may include /sdcard prefix; strip that for the download param to let openFileWithPrefixes try it
    String rel = saved;
    if (rel.startsWith("/sdcard/")) rel = rel.substring(strlen("/sdcard/"));
    if (rel.startsWith("/")) rel = rel.substring(1);
    resp += "Download URL: /download?file=" + rel;
    server.send(200, "text/plain", resp);
  } else {
    server.send(500, "text/plain", "Capture failed or SD not mounted");
  }
}

static void handleSdStatus() {
  String s = sdws_getStatus();
  server.send(200, "text/plain", s);
}

void sdws_begin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/snap", HTTP_GET, handleSnap);
  server.on("/sd_status", HTTP_GET, handleSdStatus);
  server.begin();
  Serial.println("HTTP server started.");
}

void sdws_handleClient() {
  server.handleClient();
}

void sdws_setMaxFilesToKeep(size_t maxFiles) {
  s_maxFilesToKeep = maxFiles;
}

void sdws_enforceRetentionPolicy() {
  if (s_maxFilesToKeep == 0) return;

  // detect best root to remove from
  String root = detectSdRoot();
  if (root.length() == 0) return;

  File dir = SD_MMC.open(root.c_str());
  if(!dir) return;
  std::vector<String> files;
  while(true){
    File entry = dir.openNextFile();
    if(!entry) break;
    if(!entry.isDirectory()){
      // entry.name() can be relative to mount; save basename only
      String n = String(entry.name());
      int lastSlash = n.lastIndexOf('/');
      if (lastSlash >= 0) n = n.substring(lastSlash + 1);
      files.push_back(n);
    }
    entry.close();
  }
  dir.close();

  if(files.size() <= s_maxFilesToKeep) return;
  std::sort(files.begin(), files.end()); // timestamp-style names sort chronologically
  size_t toRemove = files.size() - s_maxFilesToKeep;
  for(size_t i=0;i<toRemove;i++){
    String p = root + "/" + files[i];
    Serial.printf("Removing old file: %s\n", p.c_str());
    SD_MMC.remove(p.c_str());
  }
}

String sdws_getStatus() {
  String out;
  out += "SD mounted: ";
  out += (SD_MMC.cardType() == CARD_NONE) ? "no\n" : "yes\n";
  out += "Detected mount root: ";
  String root = detectSdRoot();
  if (root.length()) out += root + "\n";
  else out += "(none)\n";
  out += "Card type: ";
  uint8_t ct = SD_MMC.cardType();
  if (ct == CARD_NONE) out += "CARD_NONE\n";
  else if (ct == CARD_MMC) out += "MMC\n";
  else if (ct == CARD_SD) out += "SDSC\n";
  else if (ct == CARD_SDHC) out += "SDHC/SDXC\n";
  else out += "UNKNOWN\n";
  return out;
}

// New: serial debug listing - recursively prints files and sizes to Serial
static void printDirectorySerial(File dir, const String& prefix) {
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;
    String name = String(entry.name());
    String fullPath;
    if (prefix.length()) fullPath = prefix + "/" + name;
    else fullPath = name;
    if (entry.isDirectory()) {
      Serial.printf("DIR  : %s\n", fullPath.c_str());
      // recurse into directory
      File sub = SD_MMC.open(fullPath.c_str());
      if (sub) {
        printDirectorySerial(sub, fullPath);
        sub.close();
      } else {
        Serial.printf("  Failed to open subdir: %s\n", fullPath.c_str());
      }
    } else {
      Serial.printf("FILE : %s  (%u bytes)\n", fullPath.c_str(), entry.size());
    }
    entry.close();
  }
}

void sdws_debugList() {
  Serial.println("sdws_debugList: Scanning SD for files...");
  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("  SD_MMC reports no card (CARD_NONE).");
    return;
  }
  String root = detectSdRoot();
  if (root.length() == 0) {
    Serial.println("  No mount root detected (no files found or unable to access / and /sdcard).");
    // still attempt to open "/" and "/sdcard" for debugging
    File tryRoot = SD_MMC.open("/");
    if (tryRoot) {
      Serial.println("  Root '/' opened successfully but no files found.");
      tryRoot.close();
    } else {
      Serial.println("  Unable to open root '/'.");
    }
    File trySd = SD_MMC.open("/sdcard");
    if (trySd) {
      Serial.println("  '/sdcard' opened successfully but no files found.");
      trySd.close();
    } else {
      Serial.println("  Unable to open '/sdcard'.");
    }
    return;
  }

  Serial.printf("  Detected mount root: %s\n", root.c_str());
  File dir = SD_MMC.open(root.c_str());
  if (!dir) {
    Serial.printf("  Unable to open detected root: %s\n", root.c_str());
    return;
  }
  printDirectorySerial(dir, root == "/" ? String("") : root);
  dir.close();

  // Optionally print free/total space if API available (not standard in SD_MMC Arduino)
  // The Arduino SD_MMC API doesn't expose free space easily; if you have FATFS available you could print it here.
  Serial.println("sdws_debugList: scan complete.");
}
