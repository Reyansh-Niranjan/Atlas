#include <Arduino.h>
#include <SafeGithubOTA.h>
#include "ota.h"
#include "tft_ui.h"
#include "live_mic_stt.h"

#ifndef FW_VERSION
#define FW_VERSION "0.0.14"
#endif

namespace {

struct AppConfig {
  static constexpr uint8_t kSdCsPin = 10;
  static constexpr uint32_t kSdSpiFrequencyHz = 10000000;
  static constexpr const char *kLogoSdPathPrimary = "R2_Reyansh-LOGO.jpg";
  static constexpr const char *kLogoSdPathAlt = "assets/R2_Reyansh-LOGO.jpg";
  static constexpr uint32_t kSttIntervalMs = 5000;
  static constexpr uint32_t kLogoHoldMs = 1200;
  static constexpr uint32_t kLoopDelayMs = 50;
};

struct AppState {
  bool liveSttReady = false;
  uint32_t lastSttMs = 0;
};

SafeGithubOTA ota;
TftUi ui;
AppState state;

void logFirmwareVersion() {
  ui.logf("[BOOT] FW version: %s", FW_VERSION);
}

bool initStorageAndLogo() {
  const bool sdReady = ui.initSdCard(AppConfig::kSdCsPin, AppConfig::kSdSpiFrequencyHz);
  if (sdReady) {
    if (ui.showLogoFromSdCenteredHalfScreen(AppConfig::kLogoSdPathPrimary,
                                            AppConfig::kLogoSdPathAlt)) {
      delay(AppConfig::kLogoHoldMs);
    } else {
      ui.logf("[LOGO] Display skipped");
    }
  }

  return sdReady;
}

void initLiveStt() {
  String liveInitErr;
  state.liveSttReady = LiveMicStt::begin(liveInitErr);
  if (state.liveSttReady) {
    ui.logf("[STT] Live mic ready");
    ui.setStatusLine("STT: ready");
    state.lastSttMs = millis();
    return;
  }

  Serial.printf("[STT] Mic init error: %s\n", liveInitErr.c_str());
  ui.logf("[STT] Mic init error");
  ui.setStatusLine("STT: init failed");
}

void serviceLiveStt(uint32_t nowMs) {
  if (!state.liveSttReady || (nowMs - state.lastSttMs) < AppConfig::kSttIntervalMs) {
    return;
  }

  state.lastSttMs = nowMs;

  String sttText;
  String sttError;
  ui.setStatusLine("STT: Listening...");
  ui.logf("[STT] Listening...");
  if (LiveMicStt::transcribeWindow(sttText, sttError)) {
    Serial.printf("[STT] %s\n", sttText.c_str());
    ui.logf("[STT] %s", sttText.c_str());
    ui.setStatusLine("STT: Idle");
    return;
  }

  Serial.printf("[STT] Error: %s\n", sttError.c_str());
  ui.logf("[STT] Error");
  ui.setStatusLine("STT: Error");
}

void otaUiLog(const char *message) {
  ui.logf("%s", message);
}

void otaUiProgress(uint8_t pct) {
  ui.setStatusLinef("OTA: %u%%", pct);
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  ui.initBootUi();
  logFirmwareVersion();

  const bool sdReady = initStorageAndLogo();

  ui.initOtaUi();
  logFirmwareVersion();
  ui.setStatusLine("OTA: init");
  if (!sdReady) {
    ui.logf("[BOOT] SD unavailable, continuing OTA flow");
  }

  if (!OtaFlow::startAndConnectWifi(ota, FW_VERSION, otaUiLog, otaUiProgress)) {
    ui.logf("[BOOT] OTA start/connect failed; continuing main program");
    ui.setStatusLine("OTA: offline");
  } else if (!OtaFlow::checkReleaseAndFlash(ota)) {
    ui.logf("[BOOT] OTA check failed; continuing main program");
    ui.setStatusLine("OTA: check failed");
  } else {
    ui.setStatusLine("OTA: up to date");
  }

  ui.logf("[BOOT] Setup Complete");
  ui.setStatusLine("STT: init");
  initLiveStt();
}

void loop() {
  ota.loop();
  serviceLiveStt(millis());

  delay(AppConfig::kLoopDelayMs);
}
