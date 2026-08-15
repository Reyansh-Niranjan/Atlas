#include "tft_ui.h"

#include <SD.h>
#include <SPI.h>
#include <TJpg_Decoder.h>
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>

namespace {

constexpr int16_t kBootLogStartY = 12;
constexpr int16_t kBootLogLineHeight = 10;
constexpr int16_t kOtaHeaderY = 24;
constexpr int16_t kOtaDividerY = 34;
constexpr int16_t kOtaLiveLogsY = 52;
constexpr int16_t kOtaStatusLineY = 70;
constexpr int16_t kOtaLogStartY = 86;
constexpr int16_t kOtaLogLineHeight = 18;
constexpr int16_t kLogAreaBottomPadding = 8;
constexpr size_t kLogBufferSize = 192;
constexpr uint16_t kOpaqueAlpha = 255;
constexpr uint16_t kStatusLineColor = TFT_YELLOW;
constexpr uint8_t kFadeStartAlpha = 32;
constexpr uint8_t kFadeStep = 32;
constexpr uint16_t kFadeFrameDelayMs = 35;
constexpr size_t kJpegTilePixels = 16U * 16U;
constexpr std::array<uint8_t, 4> kJpgScales{{1, 2, 4, 8}};

TftUi *g_activeUi = nullptr;

void initDisplay(TFT_eSPI &tft) {
  tft.init();
  tft.setRotation(1);
  tft.writecommand(0x36);
  tft.writedata(0x40);
  tft.fillScreen(TFT_BLACK);
  tft.setTextWrap(true, false);
}

uint8_t estimateWrappedLines(const TFT_eSPI &tft, const char *text) {
  if (text == nullptr || text[0] == '\0') {
    return 1;
  }

  const int16_t screenW = tft.width();
  if (screenW <= 0) {
    return 1;
  }

  const int16_t textW = tft.textWidth(text);
  const int16_t wrappedLines = (textW + screenW - 1) / screenW;
  return static_cast<uint8_t>(std::max<int16_t>(1, wrappedLines));
}

uint8_t chooseJpgScale(uint16_t jpgW, uint16_t jpgH, int16_t screenW, int16_t screenH) {
  constexpr uint8_t kFallbackScale = 8;
  const uint16_t maxTargetW = static_cast<uint16_t>(screenW / 2);
  const uint16_t maxTargetH = static_cast<uint16_t>(screenH / 2);

  uint8_t chosenScale = kFallbackScale;
  uint32_t bestArea = 0;
  for (const uint8_t scale : kJpgScales) {
    uint16_t scaledW = static_cast<uint16_t>(jpgW / scale);
    uint16_t scaledH = static_cast<uint16_t>(jpgH / scale);
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
        chosenScale = scale;
      }
    }
  }

  return chosenScale;
}

class ActiveUiGuard {
public:
  explicit ActiveUiGuard(TftUi *ui) : previous(g_activeUi) {
    g_activeUi = ui;
  }

  ~ActiveUiGuard() {
    g_activeUi = previous;
  }

private:
  TftUi *previous;
};

}

TftUi::TftUi()
    : tft(),
      jpegFadeAlpha(kOpaqueAlpha),
      logCursorY(kBootLogStartY),
      logLineHeight(kBootLogLineHeight),
      logStartY(kBootLogStartY),
      logAreaTop(0),
      statusLineY(-1),
      statusLineHeight(0) {}

uint8_t TftUi::wrappedLineCount(const char *text) {
  return estimateWrappedLines(tft, text);
}

void TftUi::initBootUi() {
  initDisplay(tft);

  tft.setFreeFont(nullptr);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  logStartY = kBootLogStartY;
  logLineHeight = kBootLogLineHeight;
  logAreaTop = 0;
  statusLineY = -1;
  statusLineHeight = 0;
  logCursorY = logStartY;
}

void TftUi::initOtaUi() {
  initDisplay(tft);

  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setCursor(0, kOtaHeaderY);
  tft.println("EduScraper OTA Test");

  tft.drawFastHLine(0, kOtaDividerY, tft.width(), TFT_DARKGREY);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setCursor(0, kOtaLiveLogsY);
  tft.println("Live logs:");

  tft.setFreeFont(&FreeSansBold9pt7b);
  logStartY = kOtaLogStartY;
  logLineHeight = kOtaLogLineHeight;
  logAreaTop = kOtaLogStartY;
  statusLineY = kOtaStatusLineY;
  statusLineHeight = tft.fontHeight();
  logCursorY = logStartY;
}

void TftUi::logf(const char *fmt, ...) {
  char line[kLogBufferSize];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);

  const uint8_t lineCount = wrappedLineCount(line);
  const int requiredHeight = static_cast<int>(lineCount) * logLineHeight;

  if (logCursorY + requiredHeight > tft.height() - kLogAreaBottomPadding) {
    clearLogArea();
    logCursorY = logStartY;
    tft.setCursor(0, logCursorY);
    tft.print("...");
    logCursorY += logLineHeight;
  }

  tft.setCursor(0, logCursorY);
  tft.println(line);
  logCursorY += requiredHeight;
}

void TftUi::setStatusLine(const char *text) {
  if (statusLineY < 0) {
    return;
  }

  clearStatusLine();
  tft.setTextColor(kStatusLineColor, TFT_BLACK);
  tft.setCursor(0, statusLineY);
  tft.println(text == nullptr ? "" : text);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
}

void TftUi::setStatusLinef(const char *fmt, ...) {
  char line[kLogBufferSize];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  setStatusLine(line);
}

void TftUi::clearLogArea() {
  const int16_t top = std::max<int16_t>(0, static_cast<int16_t>(logAreaTop));
  tft.fillRect(0, top, tft.width(), tft.height() - top, TFT_BLACK);
}

void TftUi::clearStatusLine() {
  if (statusLineY < 0) {
    return;
  }

  const int16_t height = statusLineHeight > 0 ? static_cast<int16_t>(statusLineHeight)
                                              : static_cast<int16_t>(tft.fontHeight());
  int16_t top = static_cast<int16_t>(statusLineY - height);
  if (top < 0) {
    top = 0;
  }
  tft.fillRect(0, top, tft.width(), height + 2, TFT_BLACK);
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

  if (jpegFadeAlpha == kOpaqueAlpha && x >= 0 && y >= 0 && right <= screenW && bottom <= screenH) {
    tft.pushImage(x, y, w, h, bitmap);
    return true;
  }

  std::array<uint16_t, kJpegTilePixels> blended{};

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

  const size_t pxCount = static_cast<size_t>(copyW) * static_cast<size_t>(copyH);
  if (pxCount > blended.size()) {
    return false;
  }

  uint16_t outIndex = 0;
  for (int16_t row = 0; row < copyH; ++row) {
    const uint16_t *srcRow = bitmap + (static_cast<size_t>(srcY + row) * w) + srcX;
    for (int16_t col = 0; col < copyW; ++col) {
      const uint16_t src = srcRow[col];
      uint16_t r = static_cast<uint16_t>((src >> 11) & 0x1F);
      uint16_t g = static_cast<uint16_t>((src >> 5) & 0x3F);
      uint16_t b = static_cast<uint16_t>(src & 0x1F);

      r = static_cast<uint16_t>((r * jpegFadeAlpha) / 255U);
      g = static_cast<uint16_t>((g * jpegFadeAlpha) / 255U);
      b = static_cast<uint16_t>((b * jpegFadeAlpha) / 255U);

      blended[outIndex++] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
    }
  }

  tft.pushImage(drawX, drawY, static_cast<uint16_t>(copyW), static_cast<uint16_t>(copyH), blended.data());
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
  auto resolvePath = [](const char *path, String &resolved) -> bool {
    if (path == nullptr || path[0] == '\0') {
      return false;
    }
    if (SD.exists(path)) {
      resolved = path;
      return true;
    }
    if (path[0] != '/') {
      String withSlash = "/";
      withSlash += path;
      if (SD.exists(withSlash)) {
        resolved = withSlash;
        return true;
      }
    }
    return false;
  };

  logf("[LOGO] Try: %s", primaryPath == nullptr ? "(null)" : primaryPath);
  logf("[LOGO] Try: %s", altPath == nullptr ? "(null)" : altPath);

  String logoPath;
  if (!resolvePath(primaryPath, logoPath) && !resolvePath(altPath, logoPath)) {
    logf("[LOGO] Missing file on SD");
    return false;
  }

  logf("[LOGO] Using: %s", logoPath.c_str());

  uint16_t jpgW = 0;
  uint16_t jpgH = 0;
  if (TJpgDec.getSdJpgSize(&jpgW, &jpgH, logoPath.c_str()) != JDR_OK || jpgW == 0 || jpgH == 0) {
    logf("[LOGO] Failed reading JPG size");
    return false;
  }

  const int16_t screenW = tft.width();
  const int16_t screenH = tft.height();
  const uint8_t chosenScale = chooseJpgScale(jpgW, jpgH, screenW, screenH);

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
  ActiveUiGuard activeGuard(this);
  TJpgDec.setCallback(jpegOutputThunk);

  tft.fillScreen(TFT_BLACK);
  jpegFadeAlpha = kOpaqueAlpha;
  for (int alpha = kFadeStartAlpha; alpha < kOpaqueAlpha; alpha += kFadeStep) {
    jpegFadeAlpha = static_cast<uint8_t>(alpha);
    TJpgDec.drawSdJpg(drawX, drawY, logoPath.c_str());
    delay(kFadeFrameDelayMs);
  }

  jpegFadeAlpha = kOpaqueAlpha;
  TJpgDec.drawSdJpg(drawX, drawY, logoPath.c_str());
  return true;
}
