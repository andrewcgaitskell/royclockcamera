#include "sd_http_server.h"
#include <WebServer.h>
#include <FS.h>
#include "SD_MMC.h"
#include <vector>
#include <algorithm>

// forward declaration of capture function implemented in main sketch
extern String captureAndSave();

static WebServer server(80);
static size_t s_maxFilesToKeep = 0;

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

static void printDirectoryHtml(File dir, String &out){
  while(true){
    File entry = dir.openNextFile();
    if(!entry) break;
    String name = String(entry.name());
    if(entry.isDirectory()){
      out += "<b>" + name + "/</b><br>";
      printDirectoryHtml(entry, out);
    } else {
      out += "<a href=\"/download?file=" + name + "\">" + name + "</a> (" + String(entry.size()) + " bytes)<br>";
    }
    entry.close();
  }
}

static void handleRoot(){
  String html = "<!doctype html><html><head><meta charset='utf-8'><title>ESP32-CAM SD</title></head><body>";
  html += "<h2>Files on SD card</h2>";
  File root = SD_MMC.open("/");
  if(!root){
    html += "Failed to open root directory or SD not mounted.";
  } else {
    printDirectoryHtml(root, html);
    root.close();
  }
  html += "<hr><small>Use /download?file=/path/to/file to download or /snap to take a photo now</small></body></html>";
  server.send(200, "text/html", html);
}

static void handleDownload(){
  if(!server.hasArg("file")){
    server.send(400, "text/plain", "Missing file parameter");
    return;
  }
  String filePath = server.arg("file");
  if(!filePath.startsWith("/")) filePath = "/" + filePath;

  File f = SD_MMC.open(filePath);
  if(!f || f.isDirectory()){
    server.send(404, "text/plain", "File not found");
    if(f) f.close();
    return;
  }
  String contentType = getContentType(filePath);
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + String(filePath.substring(filePath.lastIndexOf('/')+1)) + "\"");
  server.streamFile(f, contentType);
  f.close();
}

// /snap handler: triggers captureAndSave() and returns result
static void handleSnap(){
  Serial.println("HTTP /snap requested - triggering capture");
  String saved = captureAndSave();
  if (saved.length()) {
    String resp = "Saved: " + saved + "\n";
    // provide download link (relative)
    resp += "Download URL: /download?file=" + saved;
    server.send(200, "text/plain", resp);
  } else {
    server.send(500, "text/plain", "Capture failed");
  }
}

void sdws_begin() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/snap", HTTP_GET, handleSnap); // snap endpoint
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
  File root = SD_MMC.open("/");
  if(!root) return;
  std::vector<String> files;
  while(true){
    File entry = root.openNextFile();
    if(!entry) break;
    if(!entry.isDirectory()){
      files.push_back(String(entry.name()));
    }
    entry.close();
  }
  root.close();
  if(files.size() <= s_maxFilesToKeep) return;
  std::sort(files.begin(), files.end()); // timestamp-style names sort chronologically
  size_t toRemove = files.size() - s_maxFilesToKeep;
  for(size_t i=0;i<toRemove;i++){
    String p = "/" + files[i];
    Serial.printf("Removing old file: %s\n", p.c_str());
    SD_MMC.remove(p);
  }
}
