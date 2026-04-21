#include <Arduino.h>
#include <SafeGithubOTA.h>
#include "ota.h"
#include "tft_ui.h"
#include "live_mic_stt.h"

#ifndef FW_VERSION
#define FW_VERSION "0.0.14"
#endif

static const uint8_t SD_CS_PIN = 10;
static const uint32_t SD_SPI_FREQ_HZ = 10000000;
static const char *LOGO_SD_PATH = "assets/R2_Reyansh-LOGO.jpg";
static const char *LOGO_SD_PATH_ALT = "/assets/R2_Reyansh-LOGO.jpg";
static const uint32_t STT_INTERVAL_MS = 5000;

SafeGithubOTA ota;
TftUi ui;
bool g_liveSttReady = false;
uint32_t g_lastSttMs = 0;

void setup() {
  Serial.begin(115200);
  delay(200);
  ui.initBootUi();
  ui.logf("[BOOT] FW version: %s", FW_VERSION);

  const bool sdReady = ui.initSdCard(SD_CS_PIN, SD_SPI_FREQ_HZ);
  if (sdReady) {
    if (ui.showLogoFromSdCenteredHalfScreen(LOGO_SD_PATH, LOGO_SD_PATH_ALT)) {
      delay(1200);
    } else {
      ui.logf("[LOGO] Display skipped");
    }
  } else {
    ui.logf("SD FAILED!");
  }

  ui.initOtaUi();
  ui.logf("[BOOT] FW version: %s", FW_VERSION);
  if (!sdReady) {
    ui.logf("[BOOT] SD unavailable, continuing OTA flow");
  }

  if (!OtaFlow::startAndConnectWifi(ota, FW_VERSION)) {
    ui.logf("[BOOT] OTA start/connect failed; continuing main program");
    return;
  }

  if (!OtaFlow::checkReleaseAndFlash(ota)) {
    ui.logf("[BOOT] OTA check failed; continuing main program");
  }

  ui.logf("[BOOT] Setup Complete");

  String liveInitErr;
  g_liveSttReady = LiveMicStt::begin(liveInitErr);
  if (g_liveSttReady) {
    ui.logf("[STT] Live mic ready");
    g_lastSttMs = millis();
  } else {
    Serial.printf("[STT] Mic init error: %s\n", liveInitErr.c_str());
    ui.logf("[STT] Mic init error");
  }
}

void loop() {
  ota.loop();

  if (g_liveSttReady && (millis() - g_lastSttMs) >= STT_INTERVAL_MS) {
    g_lastSttMs = millis();

    String sttText;
    String sttError;
    ui.logf("[STT] Listening...");
    if (LiveMicStt::transcribeWindow(sttText, sttError)) {
      Serial.printf("[STT] %s\n", sttText.c_str());
      ui.logf("[STT] %s", sttText.c_str());
    } else {
      Serial.printf("[STT] Error: %s\n", sttError.c_str());
      ui.logf("[STT] Error");
    }
  }

  delay(50);
}
