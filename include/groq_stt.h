#pragma once

#include <Arduino.h>

namespace GroqStt {

// Sends a WAV file from SD to Groq Whisper and returns transcribed text.
bool transcribeWavFromSd(const char *sdPath, String &outText, String &outError);

// Sends raw PCM16 mono samples as WAV to Groq Whisper and returns transcribed text.
bool transcribeWavPcm16(const int16_t *samples,
						size_t sampleCount,
						uint32_t sampleRate,
						String &outText,
						String &outError);

} // namespace GroqStt
