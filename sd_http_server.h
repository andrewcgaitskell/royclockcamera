#ifndef SD_HTTP_SERVER_H
#define SD_HTTP_SERVER_H

#include <Arduino.h>

// Start/stop/loop helpers for the SD HTTP server
void sdws_begin();
void sdws_handleClient();

// Retention policy control: set 0 to disable
void sdws_setMaxFilesToKeep(size_t maxFiles);
void sdws_enforceRetentionPolicy();

// Debug/status endpoint
String sdws_getStatus(); // returns a short text status about SD mount and mountpoint

// New: print a detailed listing of files on the SD to Serial for debugging.
// This prints full paths (as detected by the module) and file sizes.
void sdws_debugList();

#endif // SD_HTTP_SERVER_H
