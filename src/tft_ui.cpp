#include "tft_ui.h"

#include <SD.h>
#include <SPI.h>
#include <TJpg_Decoder.h>
#include <cstdarg>
#include <cstdio>

namespace {
TftUi *g_activeUi = nullptr;
}

TftUi::TftUi() : tft(), jpegFadeAlpha(255), logCursorY(86), logLineHeight(18) {}

uint8_t TftUi::wrappedLineCount(const char *text) {
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

void TftUi::initBootUi() {
  tft.init();
  tft.setRotation(1);
  tft.writecommand(0x36);
  tft.writedata(0x40);
  tft.fillScreen(TFT_BLACK);
  tft.setTextWrap(true, false);

  tft.setFreeFont(nullptr);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  logCursorY = 12;
  logLineHeight = 10;
}

void TftUi::initOtaUi() {
  tft.init();
  tft.setRotation(1);
  tft.writecommand(0x36);
  tft.writedata(0x40);
  tft.fillScreen(TFT_BLACK);
  tft.setTextWrap(true, false);

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
  logLineHeight = 18;
}

void TftUi::logf(const char *fmt, ...) {
  char line[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);

  const uint8_t lineCount = wrappedLineCount(line);
  const int requiredHeight = static_cast<int>(lineCount) * logLineHeight;

  if (logCursorY + requiredHeight > tft.height() - 8) {
    tft.fillRect(0, 70, tft.width(), tft.height() - 70, TFT_BLACK);
    logCursorY = 86;
    tft.setCursor(0, logCursorY);
    tft.print("...");
    logCursorY += logLineHeight;
  }

  tft.setCursor(0, logCursorY);
  tft.println(line);
  logCursorY += requiredHeight;
}

bool TftUi::jpegOutputThunk(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (g_activeUi == nullptr) {
    return false;
  }
  return g_activeUi->jpegOutputImpl(x, y, w, h, bitmap);
}

bool TftUi::jpegOutputImpl(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  const int16_t screenW = tft.width();
  const int16_t screenH = tft.height();
  const int16_t right = x + static_cast<int16_t>(w);
  const int16_t bottom = y + static_cast<int16_t>(h);

  if (x >= screenW || y >= screenH || right <= 0 || bottom <= 0) {
    return true;
  }

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

bool TftUi::initSdCard(uint8_t csPin, uint32_t spiFreqHz) {
  logf("[SD] Initializing (CS=%u)", csPin);

  pinMode(csPin, OUTPUT);
  digitalWrite(csPin, HIGH);

  SPIClass &tftSpi = tft.getSPIinstance();
  if (!SD.begin(csPin, tftSpi, spiFreqHz)) {
    logf("[SD] Init failed");
    logf("[SD] Check card/wiring/CS=%u", csPin);
    logf("SD FAILED!");
    return false;
  }

  logf("[SD] Mounted");

  const uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    logf("[SD] No card detected");
    logf("SD FAILED!");
    return false;
  }

  const uint64_t cardSizeMb = SD.cardSize() / (1024ULL * 1024ULL);
  logf("[SD] Card size: %lluMB", static_cast<unsigned long long>(cardSizeMb));
  return true;
}

bool TftUi::showLogoFromSdCenteredHalfScreen(const char *primaryPath, const char *altPath) {
  const char *logoPath = nullptr;
  const bool hasPrimary = SD.exists(primaryPath);
  const bool hasAlt = SD.exists(altPath);

  if (hasPrimary) {
    logoPath = primaryPath;
  } else if (hasAlt) {
    logoPath = altPath;
  }

  logf("[LOGO] Try: %s", primaryPath);
  logf("[LOGO] Try: %s", altPath);

  if (logoPath == nullptr) {
    logf("[LOGO] Missing file on SD");
    return false;
  }

  logf("[LOGO] Using: %s", logoPath);

  uint16_t jpgW = 0;
  uint16_t jpgH = 0;
  if (TJpgDec.getSdJpgSize(&jpgW, &jpgH, logoPath) != JDR_OK || jpgW == 0 || jpgH == 0) {
    logf("[LOGO] Failed reading JPG size");
    return false;
  }

  const int16_t screenW = tft.width();
  const int16_t screenH = tft.height();
  const uint16_t maxTargetW = static_cast<uint16_t>(screenW / 2);
  const uint16_t maxTargetH = static_cast<uint16_t>(screenH / 2);

  uint8_t chosenScale = 8;
  const uint8_t scales[] = {1, 2, 4, 8};
  uint32_t bestArea = 0;
  for (uint8_t i = 0; i < (sizeof(scales) / sizeof(scales[0])); ++i) {
    const uint8_t candidate = scales[i];
    uint16_t scaledW = static_cast<uint16_t>(jpgW / candidate);
    uint16_t scaledH = static_cast<uint16_t>(jpgH / candidate);
    if (scaledW == 0) {
      scaledW = 1;
    }
    if (scaledH == 0) {
      scaledH = 1;
    }

    if (scaledW <= maxTargetW && scaledH <= maxTargetH) {
      const uint32_t area = static_cast<uint32_t>(scaledW) * static_cast<uint32_t>(scaledH);
      if (area >= bestArea) {
        bestArea = area;
        chosenScale = candidate;
      }
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

  logf("[LOGO] JPG: %ux%u", jpgW, jpgH);
  logf("[LOGO] Scale: 1/%u", chosenScale);
  logf("[LOGO] Draw at: %d,%d", drawX, drawY);

  TJpgDec.setJpgScale(chosenScale);
  TJpgDec.setSwapBytes(true);
  g_activeUi = this;
  TJpgDec.setCallback(jpegOutputThunk);

  tft.fillScreen(TFT_BLACK);
  jpegFadeAlpha = 255;
  TJpgDec.drawSdJpg(drawX, drawY, logoPath);
  g_activeUi = nullptr;
  return true;
}
