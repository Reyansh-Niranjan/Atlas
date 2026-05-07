#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

class TftUi {
public:
  TftUi();

  void initBootUi();
  void initOtaUi();
  void logf(const char *fmt, ...);
  void setStatusLine(const char *text);
  void setStatusLinef(const char *fmt, ...);

  [[nodiscard]]
  bool initSdCard(uint8_t csPin, uint32_t spiFreqHz);
  [[nodiscard]]
  bool showLogoFromSdCenteredHalfScreen(const char *primaryPath, const char *altPath);

private:
  TFT_eSPI tft;
  uint8_t jpegFadeAlpha;
  int logCursorY;
  int logLineHeight;
  int logStartY;
  int logAreaTop;
  int statusLineY;
  int statusLineHeight;

  uint8_t wrappedLineCount(const char *text);
  bool jpegOutputImpl(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap);
  void clearLogArea();
  void clearStatusLine();

  static bool jpegOutputThunk(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap);
};
