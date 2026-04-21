#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <SafeGithubOTA.h>
#include <SGO_Provisioning.h>
#include "secrets.h"
#include <cstring>

// Required by SafeGithubOTA for TLS + JSON parsing on ESP32 loop task.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#ifndef OTA_GITHUB_PAT
#define OTA_GITHUB_PAT ""
#endif

namespace OtaFlow {

static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
static const uint8_t WIFI_CONNECT_RETRIES = 2;

inline void copyBounded(char *dst, size_t dstSize, const char *src) {
  if (dst == nullptr || dstSize == 0) {
    return;
  }

  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }

  strncpy(dst, src, dstSize - 1);
  dst[dstSize - 1] = '\0';
}

inline bool credentialsDiffer(const SGO_Credentials &a, const SGO_Credentials &b) {
  return strcmp(a.owner, b.owner) != 0 || strcmp(a.repo, b.repo) != 0 ||
         strcmp(a.pat, b.pat) != 0 || strcmp(a.binFilename, b.binFilename) != 0;
}

inline bool provisionFromSecrets() {
  SGO_Credentials desired{};
  copyBounded(desired.owner, sizeof(desired.owner), OTA_GITHUB_OWNER);
  copyBounded(desired.repo, sizeof(desired.repo), OTA_GITHUB_REPO);
  copyBounded(desired.pat, sizeof(desired.pat), OTA_GITHUB_PAT);
  copyBounded(desired.binFilename, sizeof(desired.binFilename), OTA_BIN_FILENAME);

  if (desired.owner[0] == '\0' || desired.repo[0] == '\0' || desired.binFilename[0] == '\0') {
    Serial.println("[OTA] OTA_GITHUB_* secrets are incomplete");
    return false;
  }

  SGO_Credentials current{};
  const bool hasCurrent = SGO_Provisioning::loadCredentials(current);
  if (hasCurrent && !credentialsDiffer(current, desired)) {
    Serial.println("[OTA] Credentials already in sync");
    return true;
  }

  if (!SGO_Provisioning::saveCredentials(desired)) {
    Serial.println("[OTA] Failed to save credentials");
    return false;
  }

  Serial.println("[OTA] Credentials saved from secrets");
  return true;
}

// Big function: configure OTA and connect device to Wi-Fi.
inline bool startAndConnectWifi(SafeGithubOTA &ota, const char *fwVersion) {
  ota.setVersion(fwVersion);
  ota.setAutoCheckInterval(6 * 60 * 60);

  ota.onLog([](const char *message) {
    Serial.printf("[OTA] %s\n", message);
  });

  ota.onProgress([](uint32_t written, uint32_t total) {
    if (total == 0) {
      return;
    }

    static uint8_t lastPct = 0;
    const uint8_t pct = static_cast<uint8_t>((written * 100U) / total);
    if (pct >= static_cast<uint8_t>(lastPct + 10) || pct == 100) {
      lastPct = pct;
      Serial.printf("[OTA] Progress: %u%%\n", pct);
    }
  });

  if (!provisionFromSecrets()) {
    return false;
  }

  WiFi.mode(WIFI_STA);

  for (uint8_t attempt = 1; attempt <= WIFI_CONNECT_RETRIES; ++attempt) {
    Serial.printf("[WIFI] Attempt %u/%u\n", attempt, WIFI_CONNECT_RETRIES);
    Serial.printf("[WIFI] Connecting to: %s\n", WIFI_SSID);

    WiFi.disconnect(true);
    delay(150);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    const uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < WIFI_CONNECT_TIMEOUT_MS) {
      delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WIFI] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
      const SGO_Error beginErr = ota.begin();
      Serial.printf("[OTA] begin(): %d\n", static_cast<int>(beginErr));
      Serial.printf("[OTA] begin msg: %s\n", ota.getLastError());
      return beginErr == SGO_Error::OK;
    }
  }

  Serial.printf("[WIFI] Failed to connect, status: %d\n", static_cast<int>(WiFi.status()));
  return false;
}

// If a new release is available, this will flash it. If not, program continues.
inline bool checkReleaseAndFlash(SafeGithubOTA &ota) {
  Serial.println("[OTA] Checking releases");
  const SGO_Error checkErr = ota.checkAndUpdate();
  Serial.printf("[OTA] check/update: %d\n", static_cast<int>(checkErr));
  Serial.printf("[OTA] result msg: %s\n", ota.getLastError());

  if (ota.wasRolledBack()) {
    Serial.println("[OTA] Previous firmware rolled back");
  }

  if (checkErr == SGO_Error::OK || checkErr == SGO_Error::ALREADY_CURRENT) {
    return true;
  }

  return false;
}

} // namespace OtaFlow
