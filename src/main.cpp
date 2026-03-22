#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <SafeGithubOTA.h>
#include <SGO_Provisioning.h>
#include "secrets.h"
#include <cstring>
#include <cstdarg>
#include <cstdio>

// Required by SafeGithubOTA for TLS + JSON parsing on ESP32 loop task.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

#ifndef FW_VERSION
#define FW_VERSION "0.0.1"
#endif

#ifndef OTA_GITHUB_PAT
#define OTA_GITHUB_PAT ""
#endif

static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
static const uint8_t WIFI_CONNECT_RETRIES = 2;

TFT_eSPI tft = TFT_eSPI(); // Invoke library, pins defined in User_Setup.h
SafeGithubOTA ota;

static int logCursorY = 86;
static const int LOG_LINE_HEIGHT = 18;

static void tftInitUi() {
  tft.init();
  tft.setRotation(1);
  tft.writecommand(0x36);
  tft.writedata(0x40);
  tft.fillScreen(TFT_BLACK);
  tft.setTextWrap(true, false);

  // Bold, non-italic FreeFont for a readable status header.
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(0, 24);
  tft.println("EduScraper OTA Test");

  tft.drawFastHLine(0, 34, tft.width(), TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(0, 52);
  tft.println("Live logs:");

  tft.setFreeFont(&FreeSansBold9pt7b);
  logCursorY = 86;
}

static void tftLogf(const char *fmt, ...) {
  char line[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);

  if (logCursorY > tft.height() - 8) {
    tft.fillRect(0, 70, tft.width(), tft.height() - 70, TFT_BLACK);
    logCursorY = 86;
    tft.setCursor(0, logCursorY);
    tft.print("...");
    logCursorY += LOG_LINE_HEIGHT;
  }

  tft.setCursor(0, logCursorY);
  tft.println(line);
  logCursorY += LOG_LINE_HEIGHT;
}

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
  tftLogf("[BOOT] Loading OTA config");

  SGO_Credentials desired{};
  copyBounded(desired.owner, sizeof(desired.owner), OTA_GITHUB_OWNER);
  copyBounded(desired.repo, sizeof(desired.repo), OTA_GITHUB_REPO);
  copyBounded(desired.pat, sizeof(desired.pat), OTA_GITHUB_PAT);
  copyBounded(desired.binFilename, sizeof(desired.binFilename), OTA_BIN_FILENAME);

  tftLogf("[BOOT] Repo: %s/%s", desired.owner, desired.repo);
  tftLogf("[BOOT] Asset: %s", desired.binFilename);
  tftLogf("[BOOT] PAT: %s", (desired.pat[0] == '\0') ? "empty" : "set");

  if (desired.owner[0] == '\0' || desired.repo[0] == '\0' || desired.binFilename[0] == '\0') {
    tftLogf("[ERR] OTA_GITHUB_* incomplete");
    return false;
  }

  SGO_Credentials current{};
  const bool hasCurrent = SGO_Provisioning::loadCredentials(current);
  if (hasCurrent && !credentialsDiffer(current, desired)) {
    tftLogf("[BOOT] OTA credentials already synced");
    return true;
  }

  if (!SGO_Provisioning::saveCredentials(desired)) {
    tftLogf("[ERR] Failed writing OTA credentials");
    return false;
  }

  tftLogf("[BOOT] OTA credentials saved");
  return true;
}

static bool connectWifiWithTimeout() {
  auto wifiStatusText = [](wl_status_t status) -> const char * {
    switch (status) {
    case WL_IDLE_STATUS:
      return "IDLE";
    case WL_NO_SSID_AVAIL:
      return "NO SSID";
    case WL_SCAN_COMPLETED:
      return "SCAN DONE";
    case WL_CONNECTED:
      return "CONNECTED";
    case WL_CONNECT_FAILED:
      return "CONNECT FAILED";
    case WL_CONNECTION_LOST:
      return "CONNECTION LOST";
    case WL_DISCONNECTED:
      return "DISCONNECTED";
    default:
      return "UNKNOWN";
    }
  };

  WiFi.mode(WIFI_STA);

  for (uint8_t attempt = 1; attempt <= WIFI_CONNECT_RETRIES; ++attempt) {
    tftLogf("[WIFI] Attempt %u/%u", attempt, WIFI_CONNECT_RETRIES);
    tftLogf("[WIFI] Connecting to: %s", WIFI_SSID);

    WiFi.disconnect(true);
    delay(150);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    const uint32_t startMs = millis();
    uint32_t lastProgressMs = 0;
    while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < WIFI_CONNECT_TIMEOUT_MS) {
      delay(250);
      if (millis() - lastProgressMs >= 3000) {
        lastProgressMs = millis();
        tftLogf("[WIFI] waiting... %lus", (millis() - startMs) / 1000UL);
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      return true;
    }

    tftLogf("[WIFI] Attempt failed: %s (%d)",
            wifiStatusText(WiFi.status()),
            static_cast<int>(WiFi.status()));
  }

  tftLogf("[WIFI] HINT: verify SSID/password and 2.4GHz signal");
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  tftInitUi();
  tftLogf("[BOOT] FW version: %s", FW_VERSION);

  ota.setVersion(FW_VERSION);
  ota.setAutoCheckInterval(6 * 60 * 60); // check every 6 hours
  tftLogf("[OTA] Auto-check: 6 hours");

  ota.onLog([](const char *message) {
    tftLogf("[OTA] %s", message);
  });

  ota.onProgress([](uint32_t written, uint32_t total) {
    if (total == 0) {
      return;
    }
    static uint8_t lastPct = 0;
    const uint8_t pct = static_cast<uint8_t>((written * 100U) / total);
    if (pct >= lastPct + 10 || pct == 100) {
      lastPct = pct;
      tftLogf("[OTA] Progress: %u%%", pct);
    }
  });

  if (!provisionFromSecrets()) {
    tftLogf("[BOOT] Stop: OTA config error");
    return;
  }

  if (!connectWifiWithTimeout()) {
    tftLogf("[ERR] Wi-Fi failed, status: %d", WiFi.status());
    return;
  }

  tftLogf("[WIFI] Connected");
  tftLogf("[WIFI] IP: %s", WiFi.localIP().toString().c_str());
  tftLogf("[WIFI] RSSI: %d dBm", WiFi.RSSI());
  tftLogf("[OTA] Initializing");
  const SGO_Error beginErr = ota.begin();
  tftLogf("[OTA] begin(): %d", static_cast<int>(beginErr));
  tftLogf("[OTA] begin msg: %s", ota.getLastError());

  if (ota.wasRolledBack()) {
    tftLogf("[OTA] Rolled back previous FW");
  }

  tftLogf("[OTA] Checking releases");
  const SGO_Error checkErr = ota.checkAndUpdate();
  tftLogf("[OTA] check/update: %d", static_cast<int>(checkErr));
  tftLogf("[OTA] result msg: %s", ota.getLastError());
  tftLogf("[BOOT] Setup complete");
}

void loop() {
  ota.loop();
  delay(50);
}
