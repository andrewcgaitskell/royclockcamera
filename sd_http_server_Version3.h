#ifndef SD_HTTP_SERVER_H
#define SD_HTTP_SERVER_H

#include <Arduino.h>

// Start/stop/loop helpers for the SD HTTP server
void sdws_begin();
void sdws_handleClient();

// Retention policy control: set 0 to disable
void sdws_setMaxFilesToKeep(size_t maxFiles);
void sdws_enforceRetentionPolicy();

// NOTE: captureAndSave() is implemented in the main sketch and is called by the /snap endpoint.
extern String captureAndSave();

#endif // SD_HTTP_SERVER_H