#include <Arduino.h>
#include <SafeGithubOTA.h>
#include "ota.h"
#include "tft_ui.h"

#ifndef FW_VERSION
#define FW_VERSION "0.0.14"
#endif

static const uint8_t SD_CS_PIN = 10;
static const uint32_t SD_SPI_FREQ_HZ = 10000000;
static const char *LOGO_SD_PATH = "assets/R2_Reyansh-LOGO.jpg";
static const char *LOGO_SD_PATH_ALT = "/assets/R2_Reyansh-LOGO.jpg";

SafeGithubOTA ota;
TftUi ui;

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
}

void loop() {
  ota.loop();
  delay(50);
}
