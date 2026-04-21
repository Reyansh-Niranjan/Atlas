#include "groq_stt.h"

#include <WiFiClientSecure.h>
#include <SD.h>
#include "secrets.h"

namespace {

#ifndef GROQ_API_KEY
#define GROQ_API_KEY ""
#endif

#ifndef GROQ_STT_MODEL
#define GROQ_STT_MODEL "whisper-large-v3-turbo"
#endif

#ifndef GROQ_STT_LANGUAGE
#define GROQ_STT_LANGUAGE "en"
#endif

static const char *kGroqHost = "api.groq.com";
static const uint16_t kGroqPort = 443;

String extractHeaderValue(const String &response, const char *headerName) {
  const String key = String("\n") + headerName + ":";
  const int start = response.indexOf(key);
  if (start < 0) {
    return "";
  }

  int valueStart = start + key.length();
  while (valueStart < response.length() && (response[valueStart] == ' ' || response[valueStart] == '\t')) {
    ++valueStart;
  }

  const int end = response.indexOf('\n', valueStart);
  if (end < 0) {
    return response.substring(valueStart);
  }

  return response.substring(valueStart, end);
}

String decodeJsonString(const String &json, int quoteStart, int quoteEnd) {
  String out;
  out.reserve(quoteEnd - quoteStart);

  bool escaped = false;
  for (int i = quoteStart; i < quoteEnd; ++i) {
    const char c = json[i];
    if (escaped) {
      switch (c) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case '\\': out += '\\'; break;
      case '"': out += '"'; break;
      default: out += c; break;
      }
      escaped = false;
      continue;
    }

    if (c == '\\') {
      escaped = true;
      continue;
    }

    out += c;
  }

  return out;
}

bool extractTextField(const String &jsonBody, String &outText) {
  const int textKey = jsonBody.indexOf("\"text\"");
  if (textKey < 0) {
    return false;
  }

  const int colon = jsonBody.indexOf(':', textKey);
  if (colon < 0) {
    return false;
  }

  int firstQuote = jsonBody.indexOf('"', colon + 1);
  if (firstQuote < 0) {
    return false;
  }

  int i = firstQuote + 1;
  bool escaped = false;
  int lastQuote = -1;
  for (; i < jsonBody.length(); ++i) {
    const char c = jsonBody[i];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') {
      lastQuote = i;
      break;
    }
  }

  if (lastQuote < 0) {
    return false;
  }

  outText = decodeJsonString(jsonBody, firstQuote + 1, lastQuote);
  return true;
}

} // namespace

namespace GroqStt {

bool transcribeWavPcm16(const int16_t *samples,
                        size_t sampleCount,
                        uint32_t sampleRate,
                        String &outText,
                        String &outError) {
  outText = "";
  outError = "";

  if (GROQ_API_KEY[0] == '\0') {
    outError = "GROQ_API_KEY is empty in secrets.h";
    return false;
  }
  if (samples == nullptr || sampleCount == 0) {
    outError = "No PCM samples provided";
    return false;
  }

  const String boundary = "----EduScraperGroqBoundary7d9f";
  const String preamble =
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
      GROQ_STT_MODEL "\r\n"
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
      GROQ_STT_LANGUAGE "\r\n"
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n"
      "json\r\n"
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"live.wav\"\r\n"
      "Content-Type: audio/wav\r\n\r\n";
  const String postamble = "\r\n--" + boundary + "--\r\n";

  const uint32_t dataBytes = static_cast<uint32_t>(sampleCount * sizeof(int16_t));
  uint8_t wavHeader[44] = {
      'R', 'I', 'F', 'F',
      0, 0, 0, 0,
      'W', 'A', 'V', 'E',
      'f', 'm', 't', ' ',
      16, 0, 0, 0,
      1, 0,
      1, 0,
      0, 0, 0, 0,
      0, 0, 0, 0,
      2, 0,
      16, 0,
      'd', 'a', 't', 'a',
      0, 0, 0, 0,
  };

  const uint32_t riffSize = 36 + dataBytes;
  wavHeader[4] = static_cast<uint8_t>(riffSize & 0xFF);
  wavHeader[5] = static_cast<uint8_t>((riffSize >> 8) & 0xFF);
  wavHeader[6] = static_cast<uint8_t>((riffSize >> 16) & 0xFF);
  wavHeader[7] = static_cast<uint8_t>((riffSize >> 24) & 0xFF);

  wavHeader[24] = static_cast<uint8_t>(sampleRate & 0xFF);
  wavHeader[25] = static_cast<uint8_t>((sampleRate >> 8) & 0xFF);
  wavHeader[26] = static_cast<uint8_t>((sampleRate >> 16) & 0xFF);
  wavHeader[27] = static_cast<uint8_t>((sampleRate >> 24) & 0xFF);

  const uint32_t byteRate = sampleRate * 2;
  wavHeader[28] = static_cast<uint8_t>(byteRate & 0xFF);
  wavHeader[29] = static_cast<uint8_t>((byteRate >> 8) & 0xFF);
  wavHeader[30] = static_cast<uint8_t>((byteRate >> 16) & 0xFF);
  wavHeader[31] = static_cast<uint8_t>((byteRate >> 24) & 0xFF);

  wavHeader[40] = static_cast<uint8_t>(dataBytes & 0xFF);
  wavHeader[41] = static_cast<uint8_t>((dataBytes >> 8) & 0xFF);
  wavHeader[42] = static_cast<uint8_t>((dataBytes >> 16) & 0xFF);
  wavHeader[43] = static_cast<uint8_t>((dataBytes >> 24) & 0xFF);

  const size_t contentLength = preamble.length() + sizeof(wavHeader) + dataBytes + postamble.length();

  WiFiClientSecure client;
  client.setInsecure();
  if (!client.connect(kGroqHost, kGroqPort)) {
    outError = "TLS connect failed to api.groq.com";
    return false;
  }

  client.printf("POST /openai/v1/audio/transcriptions HTTP/1.1\r\n");
  client.printf("Host: %s\r\n", kGroqHost);
  client.printf("Authorization: Bearer %s\r\n", GROQ_API_KEY);
  client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
  client.printf("Content-Length: %u\r\n", static_cast<unsigned>(contentLength));
  client.print("Connection: close\r\n\r\n");

  client.print(preamble);
  client.write(wavHeader, sizeof(wavHeader));
  client.write(reinterpret_cast<const uint8_t *>(samples), dataBytes);
  client.print(postamble);

  const uint32_t timeoutMs = millis() + 30000;
  while (!client.available() && client.connected() && millis() < timeoutMs) {
    delay(10);
  }

  String response;
  response.reserve(4096);
  while (client.connected() || client.available()) {
    while (client.available()) {
      response += static_cast<char>(client.read());
    }
    delay(1);
  }
  client.stop();

  if (!response.startsWith("HTTP/1.1 200")) {
    const String statusLine = response.substring(0, response.indexOf('\n'));
    const int bodyPos = response.indexOf("\r\n\r\n");
    String body = bodyPos >= 0 ? response.substring(bodyPos + 4) : "";
    body.trim();
    outError = "HTTP error: " + statusLine + " body=" + body;
    return false;
  }

  const int bodyPos = response.indexOf("\r\n\r\n");
  if (bodyPos < 0) {
    outError = "No HTTP body in response";
    return false;
  }

  String body = response.substring(bodyPos + 4);
  body.trim();
  if (!extractTextField(body, outText)) {
    outError = "Could not parse transcription text from response";
    return false;
  }

  return true;
}

bool transcribeWavFromSd(const char *sdPath, String &outText, String &outError) {
  outText = "";
  outError = "";

  if (GROQ_API_KEY[0] == '\0') {
    outError = "GROQ_API_KEY is empty in secrets.h";
    return false;
  }

  File wavFile = SD.open(sdPath, FILE_READ);
  if (!wavFile) {
    outError = String("Failed to open SD file: ") + sdPath;
    return false;
  }

  const String boundary = "----EduScraperGroqBoundary7d9f";
  const String preamble =
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
      GROQ_STT_MODEL "\r\n"
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"language\"\r\n\r\n"
      GROQ_STT_LANGUAGE "\r\n"
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n"
      "json\r\n"
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
      "Content-Type: audio/wav\r\n\r\n";

  const String postamble = "\r\n--" + boundary + "--\r\n";

  const size_t contentLength = preamble.length() + static_cast<size_t>(wavFile.size()) + postamble.length();

  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect(kGroqHost, kGroqPort)) {
    outError = "TLS connect failed to api.groq.com";
    wavFile.close();
    return false;
  }

  client.printf("POST /openai/v1/audio/transcriptions HTTP/1.1\r\n");
  client.printf("Host: %s\r\n", kGroqHost);
  client.printf("Authorization: Bearer %s\r\n", GROQ_API_KEY);
  client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
  client.printf("Content-Length: %u\r\n", static_cast<unsigned>(contentLength));
  client.print("Connection: close\r\n\r\n");

  client.print(preamble);

  uint8_t buf[1024];
  while (wavFile.available()) {
    const size_t readBytes = wavFile.read(buf, sizeof(buf));
    if (readBytes == 0) {
      break;
    }
    client.write(buf, readBytes);
  }
  wavFile.close();

  client.print(postamble);

  const uint32_t timeoutMs = millis() + 30000;
  while (!client.available() && client.connected() && millis() < timeoutMs) {
    delay(10);
  }

  String response;
  response.reserve(4096);
  while (client.connected() || client.available()) {
    while (client.available()) {
      response += static_cast<char>(client.read());
    }
    delay(1);
  }
  client.stop();

  if (!response.startsWith("HTTP/1.1 200")) {
    const String statusLine = response.substring(0, response.indexOf('\n'));
    const int bodyPos = response.indexOf("\r\n\r\n");
    String body = bodyPos >= 0 ? response.substring(bodyPos + 4) : "";
    body.trim();
    outError = "HTTP error: " + statusLine + " body=" + body;
    return false;
  }

  const int bodyPos = response.indexOf("\r\n\r\n");
  if (bodyPos < 0) {
    outError = "No HTTP body in response";
    return false;
  }

  String body = response.substring(bodyPos + 4);
  body.trim();

  if (!extractTextField(body, outText)) {
    outError = "Could not parse transcription text from response";
    return false;
  }

  return true;
}

} // namespace GroqStt
