#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

class TftUi {
public:
  TftUi();

  void initBootUi();
  void initOtaUi();
  void logf(const char *fmt, ...);

  bool initSdCard(uint8_t csPin, uint32_t spiFreqHz);
  bool showLogoFromSdCenteredHalfScreen(const char *primaryPath, const char *altPath);

private:
  TFT_eSPI tft;
  uint8_t jpegFadeAlpha;
  int logCursorY;
  int logLineHeight;

  uint8_t wrappedLineCount(const char *text);
  bool jpegOutputImpl(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap);

  static bool jpegOutputThunk(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap);
};
