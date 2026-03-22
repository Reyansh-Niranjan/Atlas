#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <SafeGithubOTA.h>
#include <SGO_Provisioning.h>
#include "secrets.h"
#include <cstring>

// Required by SafeGithubOTA for TLS + JSON parsing on ESP32 loop task.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#ifndef FW_VERSION
#define FW_VERSION "0.0.1"
#endif

#ifndef OTA_GITHUB_PAT
#define OTA_GITHUB_PAT ""
#endif

static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;

TFT_eSPI tft = TFT_eSPI(); // Invoke library, pins defined in User_Setup.h
SafeGithubOTA ota;

static void copyBounded(char* dst, size_t dstSize, const char* src) {
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

static bool credentialsDiffer(const SGO_Credentials& a, const SGO_Credentials& b) {
  return strcmp(a.owner, b.owner) != 0 || strcmp(a.repo, b.repo) != 0 ||
         strcmp(a.pat, b.pat) != 0 || strcmp(a.binFilename, b.binFilename) != 0;
}

static bool provisionFromSecrets() {
  Serial.println("[BOOT] Loading OTA credentials from secrets.h");

  SGO_Credentials desired{};
  copyBounded(desired.owner, sizeof(desired.owner), OTA_GITHUB_OWNER);
  copyBounded(desired.repo, sizeof(desired.repo), OTA_GITHUB_REPO);
  copyBounded(desired.pat, sizeof(desired.pat), OTA_GITHUB_PAT);
  copyBounded(desired.binFilename, sizeof(desired.binFilename), OTA_BIN_FILENAME);

  Serial.printf("[BOOT] OTA target repo: %s/%s | asset: %s | PAT: %s\n",
                desired.owner,
                desired.repo,
                desired.binFilename,
                (desired.pat[0] == '\0') ? "empty (public repo mode)" : "set");

  if (desired.owner[0] == '\0' || desired.repo[0] == '\0' || desired.binFilename[0] == '\0') {
    Serial.println("OTA secrets are incomplete. Fill OTA_GITHUB_* values in secrets.h");
    return false;
  }

  SGO_Credentials current{};
  const bool hasCurrent = SGO_Provisioning::loadCredentials(current);
  if (hasCurrent && !credentialsDiffer(current, desired)) {
    Serial.println("[BOOT] OTA credentials already synced in NVS.");
    return true;
  }

  if (!SGO_Provisioning::saveCredentials(desired)) {
    Serial.println("Failed to write OTA credentials to NVS.");
    return false;
  }

  Serial.println("OTA credentials saved from secrets.h");
  return true;
}

static bool connectWifiWithTimeout() {
  Serial.printf("[WIFI] Connecting to SSID: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print('.');
  }

  return WiFi.status() == WL_CONNECTED;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("================ EduScraper Test Boot ================");
  Serial.printf("[BOOT] FW version: %s\n", FW_VERSION);

  tft.init();
  tft.setRotation(1);
  tft.writecommand(0x36);
  tft.writedata(0x40);
  tft.fillScreen(TFT_BLACK);
  tft.setFreeFont(&FreeSansBoldOblique24pt7b);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(0, 50);
  tft.println("EduScraper Boot");

  ota.setVersion(FW_VERSION);
  ota.setAutoCheckInterval(6 * 60 * 60); // check every 6 hours
  Serial.println("[OTA] Auto-check interval set to 6 hours.");

  ota.onLog([](const char *message) {
    Serial.printf("[OTA-LIB] %s\n", message);
  });

  ota.onProgress([](uint32_t written, uint32_t total) {
    if (total == 0) {
      return;
    }
    static uint8_t lastPct = 0;
    const uint8_t pct = static_cast<uint8_t>((written * 100U) / total);
    if (pct >= lastPct + 10 || pct == 100) {
      lastPct = pct;
      Serial.printf("OTA progress: %u%%\n", pct);
    }
  });

  if (!provisionFromSecrets()) {
    Serial.println("[BOOT] Stopping startup due to OTA config error.");
    tft.setCursor(0, 95);
    tft.println("OTA cfg missing");
    return;
  }

  Serial.print("Connecting Wi-Fi");
  if (!connectWifiWithTimeout()) {
    Serial.println("\nWi-Fi connection failed. OTA skipped.");
    Serial.printf("[WIFI] Final status code: %d\n", WiFi.status());
    tft.setCursor(0, 140);
    tft.println("Wi-Fi failed");
    return;
  }

  Serial.println("\nWi-Fi connected.");
  Serial.printf("[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WIFI] RSSI: %d dBm\n", WiFi.RSSI());
  Serial.println("Initializing OTA...");
  const SGO_Error beginErr = ota.begin();
  Serial.printf("[OTA] begin() result: %d (%s)\n", static_cast<int>(beginErr), ota.getLastError());

  if (ota.wasRolledBack()) {
    Serial.println("Notice: previous firmware was rolled back.");
  }

  Serial.println("Checking GitHub release OTA...");
  const SGO_Error checkErr = ota.checkAndUpdate();
  Serial.printf("[OTA] checkAndUpdate() result: %d (%s)\n",
                static_cast<int>(checkErr), ota.getLastError());
  Serial.println("[BOOT] Setup complete.");
}

void loop() {
  ota.loop();
  delay(50);
}
