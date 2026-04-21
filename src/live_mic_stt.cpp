#include "live_mic_stt.h"

#include <driver/i2s.h>
#include "groq_stt.h"

namespace {

static const i2s_port_t kI2sPort = I2S_NUM_0;
static const uint32_t kSampleRateHz = 16000;
static const uint8_t kWindowSeconds = 2;
static const size_t kWindowSamples = kSampleRateHz * kWindowSeconds;

// INMP441 pin mapping from project wiring.
static const int kI2sBclkPin = 14;
static const int kI2sWsPin = 15;
static const int kI2sDataInPin = 16;

int16_t g_pcmBuffer[kWindowSamples];
bool g_i2sReady = false;

} // namespace

namespace LiveMicStt {

bool begin(String &outError) {
  outError = "";

  const i2s_config_t i2sConfig = {
      .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = static_cast<int>(kSampleRateHz),
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 256,
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
  int32_t i2sRaw[256];

  while (captured < kWindowSamples) {
    size_t bytesRead = 0;
    const esp_err_t readErr = i2s_read(kI2sPort, i2sRaw, sizeof(i2sRaw), &bytesRead, portMAX_DELAY);
    if (readErr != ESP_OK) {
      outError = String("i2s_read failed: ") + String(static_cast<int>(readErr));
      return false;
    }

    const size_t samplesRead = bytesRead / sizeof(int32_t);
    for (size_t i = 0; i < samplesRead && captured < kWindowSamples; ++i) {
      const int32_t sample32 = i2sRaw[i];
      g_pcmBuffer[captured++] = static_cast<int16_t>(sample32 >> 14);
    }
  }

  return GroqStt::transcribeWavPcm16(g_pcmBuffer, captured, kSampleRateHz, outText, outError);
}

} // namespace LiveMicStt
