#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include <SD.h>
#include <SPI.h>
#include <TJpg_Decoder.h>
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
static const uint8_t SD_CS_PIN = 10;
static const char *LOGO_SD_PATH = "assets/R2_Reyansh-LOGO.jpg";

TFT_eSPI tft = TFT_eSPI(); // Invoke library, pins defined in User_Setup.h
SafeGithubOTA ota;
static uint8_t jpegFadeAlpha = 255;

static int logCursorY = 86;
static const int LOG_LINE_HEIGHT = 18;

static uint8_t wrappedLineCount(const char *text) {
  if (text == nullptr || text[0] == '\0') {
    return 1;
  }

  const int16_t widthPx = tft.textWidth(text);
  const int16_t screenW = tft.width();
  if (screenW <= 0) {
    return 1;
  }

  int16_t lines = (widthPx + screenW - 1) / screenW;
  if (lines < 1) {
    lines = 1;
  }
  return static_cast<uint8_t>(lines);
}

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

  const uint8_t lineCount = wrappedLineCount(line);
  const int requiredHeight = static_cast<int>(lineCount) * LOG_LINE_HEIGHT;

  if (logCursorY + requiredHeight > tft.height() - 8) {
    tft.fillRect(0, 70, tft.width(), tft.height() - 70, TFT_BLACK);
    logCursorY = 86;
    tft.setCursor(0, logCursorY);
    tft.print("...");
    logCursorY += LOG_LINE_HEIGHT;
  }

  tft.setCursor(0, logCursorY);
  tft.println(line);
  logCursorY += requiredHeight;
}

static bool tftJpegOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  const int16_t screenW = tft.width();
  const int16_t screenH = tft.height();
  const int16_t right = x + static_cast<int16_t>(w);
  const int16_t bottom = y + static_cast<int16_t>(h);

  if (x >= screenW || y >= screenH || right <= 0 || bottom <= 0) {
    return true;
  }

  // Fast path: no fade math when block is fully visible at full opacity.
  if (jpegFadeAlpha == 255 && x >= 0 && y >= 0 && right <= screenW && bottom <= screenH) {
    tft.pushImage(x, y, w, h, bitmap);
    return true;
  }

  static uint16_t blended[16 * 16];
  const uint16_t blendedCapacity = static_cast<uint16_t>(sizeof(blended) / sizeof(blended[0]));

  int16_t drawX = x;
  int16_t drawY = y;
  int16_t srcX = 0;
  int16_t srcY = 0;
  int16_t copyW = static_cast<int16_t>(w);
  int16_t copyH = static_cast<int16_t>(h);

  if (drawX < 0) {
    srcX = -drawX;
    copyW += drawX;
    drawX = 0;
  }
  if (drawY < 0) {
    srcY = -drawY;
    copyH += drawY;
    drawY = 0;
  }

  if (drawX + copyW > screenW) {
    copyW = screenW - drawX;
  }
  if (drawY + copyH > screenH) {
    copyH = screenH - drawY;
  }

  if (copyW <= 0 || copyH <= 0) {
    return true;
  }

  const uint16_t pxCount = static_cast<uint16_t>(copyW * copyH);
  if (pxCount > blendedCapacity) {
    return false;
  }

  uint16_t outIndex = 0;
  for (int16_t row = 0; row < copyH; ++row) {
    const int16_t srcRow = srcY + row;
    for (int16_t col = 0; col < copyW; ++col) {
      const int16_t srcCol = srcX + col;
      const uint16_t src = bitmap[srcRow * static_cast<int16_t>(w) + srcCol];
      uint16_t r = static_cast<uint16_t>((src >> 11) & 0x1F);
      uint16_t g = static_cast<uint16_t>((src >> 5) & 0x3F);
      uint16_t b = static_cast<uint16_t>(src & 0x1F);

      r = static_cast<uint16_t>((r * jpegFadeAlpha) / 255U);
      g = static_cast<uint16_t>((g * jpegFadeAlpha) / 255U);
      b = static_cast<uint16_t>((b * jpegFadeAlpha) / 255U);

      blended[outIndex++] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
    }
  }

  tft.pushImage(drawX, drawY, static_cast<uint16_t>(copyW), static_cast<uint16_t>(copyH), blended);
  return true;
}

static bool initSdCard() {
  tftLogf("[SD] Initializing (CS=%u)", SD_CS_PIN);
  if (!SD.begin(SD_CS_PIN)) {
    tftLogf("[SD] Init failed");
    return false;
  }

  const uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    tftLogf("[SD] No card detected");
    return false;
  }

  const uint64_t cardSizeMb = SD.cardSize() / (1024ULL * 1024ULL);
  tftLogf("[SD] Card size: %lluMB", static_cast<unsigned long long>(cardSizeMb));
  return true;
}

static bool showLogoFromSdWithFade() {
  tftLogf("[LOGO] SD file: %s", LOGO_SD_PATH);

  if (!SD.exists(LOGO_SD_PATH)) {
    tftLogf("[LOGO] Missing file on SD");
    return false;
  }

  uint16_t jpgW = 0;
  uint16_t jpgH = 0;
  if (TJpgDec.getSdJpgSize(&jpgW, &jpgH, LOGO_SD_PATH) != JDR_OK || jpgW == 0 || jpgH == 0) {
    tftLogf("[LOGO] Failed reading JPG size");
    return false;
  }

  const int16_t screenW = tft.width();
  const int16_t screenH = tft.height();

  // Pick the largest decoder reduction that still covers the display.
  uint8_t chosenScale = 1;
  const uint8_t scales[] = {8, 4, 2, 1};
  for (uint8_t i = 0; i < (sizeof(scales) / sizeof(scales[0])); ++i) {
    const uint8_t candidate = scales[i];
    const uint16_t scaledW = static_cast<uint16_t>(jpgW / candidate);
    const uint16_t scaledH = static_cast<uint16_t>(jpgH / candidate);
    if (scaledW >= static_cast<uint16_t>(screenW) && scaledH >= static_cast<uint16_t>(screenH)) {
      chosenScale = candidate;
      break;
    }
  }

  uint16_t scaledW = static_cast<uint16_t>(jpgW / chosenScale);
  uint16_t scaledH = static_cast<uint16_t>(jpgH / chosenScale);
  if (scaledW == 0) {
    scaledW = 1;
  }
  if (scaledH == 0) {
    scaledH = 1;
  }

  const int16_t drawX = static_cast<int16_t>((screenW - static_cast<int16_t>(scaledW)) / 2);
  const int16_t drawY = static_cast<int16_t>((screenH - static_cast<int16_t>(scaledH)) / 2);

  tftLogf("[LOGO] JPG: %ux%u", jpgW, jpgH);
  tftLogf("[LOGO] Scale: 1/%u", chosenScale);
  tftLogf("[LOGO] Draw at: %d,%d", drawX, drawY);

  TJpgDec.setJpgScale(chosenScale);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tftJpegOutput);

  tft.fillScreen(TFT_BLACK);
  for (uint8_t alpha = 32; alpha <= 255; alpha = static_cast<uint8_t>(alpha + 32)) {
    jpegFadeAlpha = alpha;
    TJpgDec.drawSdJpg(drawX, drawY, LOGO_SD_PATH);
    delay(60);
    if (alpha == 224) {
      break;
    }
  }

  jpegFadeAlpha = 255;
  TJpgDec.drawSdJpg(drawX, drawY, LOGO_SD_PATH);
  return true;
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
  tftLogf("[BOOT] Setup Complete");

  if (initSdCard()) {
    if (!showLogoFromSdWithFade()) {
      tftLogf("[LOGO] Display skipped");
    }
  }
}

void loop() {
  ota.loop();
  delay(50);
}
