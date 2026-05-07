#include "live_mic_stt.h"

#include <algorithm>
#include <array>
#include <driver/i2s.h>
#include "groq_stt.h"

namespace {

constexpr i2s_port_t kI2sPort = I2S_NUM_0;
constexpr uint32_t kSampleRateHz = 16000;
constexpr uint8_t kWindowSeconds = 2;
constexpr size_t kWindowSamples = kSampleRateHz * kWindowSeconds;
constexpr size_t kI2sChunkSamples = 256;
constexpr int kSampleShiftBits = 14;
constexpr uint16_t kI2sDmaBufCount = 8;
constexpr uint16_t kI2sDmaBufLen = 256;

// INMP441 pin mapping from project wiring.
constexpr int kI2sBclkPin = 14;
constexpr int kI2sWsPin = 15;
constexpr int kI2sDataInPin = 16;

static_assert(kWindowSamples > 0, "Window must contain samples");

std::array<int16_t, kWindowSamples> g_pcmBuffer{};
bool g_i2sReady = false;

bool installI2s(String &outError) {
  const i2s_config_t i2sConfig = {
      .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = static_cast<int>(kSampleRateHz),
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = kI2sDmaBufCount,
      .dma_buf_len = kI2sDmaBufLen,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0,
  };

  const i2s_pin_config_t pinConfig = {
      .bck_io_num = kI2sBclkPin,
      .ws_io_num = kI2sWsPin,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num = kI2sDataInPin,
  };

  const esp_err_t installErr = i2s_driver_install(kI2sPort, &i2sConfig, 0, nullptr);
  if (installErr != ESP_OK) {
    outError = String("i2s_driver_install failed: ") + String(static_cast<int>(installErr));
    return false;
  }

  const esp_err_t pinErr = i2s_set_pin(kI2sPort, &pinConfig);
  if (pinErr != ESP_OK) {
    i2s_driver_uninstall(kI2sPort);
    outError = String("i2s_set_pin failed: ") + String(static_cast<int>(pinErr));
    return false;
  }

  i2s_zero_dma_buffer(kI2sPort);
  return true;
}

} // namespace

namespace LiveMicStt {

bool begin(String &outError) {
  outError = "";
  if (g_i2sReady) {
    return true;
  }

  if (!installI2s(outError)) {
    return false;
  }

  g_i2sReady = true;
  return true;
}

bool transcribeWindow(String &outText, String &outError) {
  outText = "";
  outError = "";

  if (!g_i2sReady) {
    outError = "I2S is not initialized";
    return false;
  }

  size_t captured = 0;
  std::array<int32_t, kI2sChunkSamples> i2sRaw{};

  while (captured < kWindowSamples) {
    size_t bytesRead = 0;
    const esp_err_t readErr = i2s_read(kI2sPort, i2sRaw.data(), i2sRaw.size() * sizeof(i2sRaw[0]), &bytesRead, portMAX_DELAY);
    if (readErr != ESP_OK) {
      outError = String("i2s_read failed: ") + String(static_cast<int>(readErr));
      return false;
    }

    const size_t samplesRead = bytesRead / sizeof(i2sRaw[0]);
    const size_t remaining = kWindowSamples - captured;
    const size_t samplesToCopy = std::min(samplesRead, remaining);
    for (size_t i = 0; i < samplesToCopy; ++i) {
      g_pcmBuffer[captured + i] = static_cast<int16_t>(i2sRaw[i] >> kSampleShiftBits);
    }
    captured += samplesToCopy;
  }

  return GroqStt::transcribeWavPcm16(g_pcmBuffer.data(), captured, kSampleRateHz, outText, outError);
}

} // namespace LiveMicStt
