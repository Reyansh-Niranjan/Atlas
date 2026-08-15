#include "groq_stt.h"

#include <ArduinoJson.h>
#include <SD.h>
#include <WiFiClientSecure.h>
#include <array>
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

constexpr char kGroqHost[] = "api.groq.com";
constexpr uint16_t kGroqPort = 443;
constexpr char kTranscriptionsPath[] = "/openai/v1/audio/transcriptions";
constexpr char kMultipartBoundary[] = "----EduScraperGroqBoundary7d9f";
constexpr uint32_t kHttpTimeoutMs = 30000;
constexpr size_t kJsonDocSize = 2048;
constexpr size_t kWavHeaderSize = 44;
constexpr size_t kMultipartChunkSize = 1024;

String buildMultipartPreamble(const char *filename) {
  String preamble;
  preamble.reserve(320);
  preamble += "--";
  preamble += kMultipartBoundary;
  preamble += "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n";
  preamble += GROQ_STT_MODEL;
  preamble += "\r\n--";
  preamble += kMultipartBoundary;
  preamble += "\r\nContent-Disposition: form-data; name=\"language\"\r\n\r\n";
  preamble += GROQ_STT_LANGUAGE;
  preamble += "\r\n--";
  preamble += kMultipartBoundary;
  preamble += "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n--";
  preamble += kMultipartBoundary;
  preamble += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"";
  preamble += filename;
  preamble += "\"\r\nContent-Type: audio/wav\r\n\r\n";
  return preamble;
}

String buildMultipartPostamble() {
  String postamble;
  postamble.reserve(sizeof(kMultipartBoundary) + 8);
  postamble += "\r\n--";
  postamble += kMultipartBoundary;
  postamble += "--\r\n";
  return postamble;
}

void writeLittleEndian32(uint8_t *dest, size_t offset, uint32_t value) {
  dest[offset + 0] = static_cast<uint8_t>(value & 0xFF);
  dest[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
  dest[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
  dest[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
}

void buildWavHeader(uint8_t header[kWavHeaderSize], uint32_t sampleRate, uint32_t dataBytes) {
  header[0] = 'R';
  header[1] = 'I';
  header[2] = 'F';
  header[3] = 'F';
  header[8] = 'W';
  header[9] = 'A';
  header[10] = 'V';
  header[11] = 'E';
  header[12] = 'f';
  header[13] = 'm';
  header[14] = 't';
  header[15] = ' ';
  header[16] = 16;
  header[20] = 1;
  header[22] = 1;
  header[32] = 2;
  header[34] = 16;
  header[36] = 'd';
  header[37] = 'a';
  header[38] = 't';
  header[39] = 'a';

  const uint32_t riffSize = 36 + dataBytes;
  writeLittleEndian32(header, 4, riffSize);
  writeLittleEndian32(header, 24, sampleRate);
  writeLittleEndian32(header, 28, sampleRate * 2U);
  writeLittleEndian32(header, 40, dataBytes);
}

bool writeAll(WiFiClientSecure &client, const uint8_t *data, size_t size, String &outError) {
  size_t offset = 0;
  constexpr size_t kMaxChunkSize = 256;
  while (offset < size) {
    const size_t remaining = size - offset;
    const size_t chunkSize = (remaining > kMaxChunkSize) ? kMaxChunkSize : remaining;
    const size_t written = client.write(data + offset, chunkSize);
    if (written == 0 || written != chunkSize) {
      outError = "Failed to write request body";
      return false;
    }
    offset += written;
  }
  return true;
}

bool writeText(WiFiClientSecure &client, const String &text, String &outError) {
  return writeAll(client, reinterpret_cast<const uint8_t *>(text.c_str()), text.length(), outError);
}

bool readStatusLine(WiFiClientSecure &client, String &outStatusLine, String &outError) {
  outStatusLine = client.readStringUntil('\n');
  outStatusLine.trim();
  if (outStatusLine.isEmpty()) {
    outError = "Empty HTTP status line";
    return false;
  }
  return true;
}

bool skipHttpHeaders(WiFiClientSecure &client, String &outError) {
  constexpr uint32_t kHeaderTimeoutMs = 5000;
  const uint32_t startTime = millis();
  while (client.connected() && (millis() - startTime < kHeaderTimeoutMs)) {
    String line = client.readStringUntil('\n');
    if (line.isEmpty()) {
      if (!client.connected()) {
        outError = "Connection closed before HTTP headers";
        return false;
      }
      continue;
    }
    line.trim();
    if (line.isEmpty()) {
      return true;
    }
  }

  outError = "Timed out waiting for HTTP headers";
  return false;
}

bool parseHttpStatusCode(const String &statusLine, int &outStatusCode) {
  const int firstSpace = statusLine.indexOf(' ');
  if (firstSpace < 0) {
    return false;
  }

  const int secondSpace = statusLine.indexOf(' ', firstSpace + 1);
  const String codeText = secondSpace < 0 ? statusLine.substring(firstSpace + 1) : statusLine.substring(firstSpace + 1, secondSpace);
  outStatusCode = codeText.toInt();
  return outStatusCode > 0;
}

bool parseTranscriptionBody(Stream &stream, String &outText, String &outError) {
  StaticJsonDocument<kJsonDocSize> doc;
  const DeserializationError err = deserializeJson(doc, stream);
  if (err) {
    outError = String("JSON parse failed: ") + err.c_str();
    return false;
  }

  const char *text = doc["text"].as<const char *>();
  if (text == nullptr || text[0] == '\0') {
    outError = "Missing transcription text";
    return false;
  }

  outText = String(text);
  return true;
}

class ScopedClientStop {
public:
  explicit ScopedClientStop(WiFiClientSecure &client) : client(client) {}
  ~ScopedClientStop() { client.stop(); }

private:
  WiFiClientSecure &client;
};

class ScopedFileClose {
public:
  explicit ScopedFileClose(File &file) : file(file) {}
  ~ScopedFileClose() {
    if (file) {
      file.close();
    }
  }

private:
  File &file;
};

template <typename BodyWriter>
bool executeTranscriptionRequest(const String &preamble,
                                 size_t bodyBytes,
                                 BodyWriter &&writeBody,
                                 String &outText,
                                 String &outError) {
  WiFiClientSecure client;
  ScopedClientStop clientGuard(client);
  client.setInsecure();
  client.setTimeout(kHttpTimeoutMs);

  if (!client.connect(kGroqHost, kGroqPort)) {
    outError = "TLS connect failed to api.groq.com";
    return false;
  }

  const String postamble = buildMultipartPostamble();
  const size_t contentLength = preamble.length() + bodyBytes + postamble.length();

  client.printf("POST %s HTTP/1.1\r\n", kTranscriptionsPath);
  client.printf("Host: %s\r\n", kGroqHost);
  client.printf("Authorization: Bearer %s\r\n", GROQ_API_KEY);
  client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", kMultipartBoundary);
  client.printf("Content-Length: %u\r\n", static_cast<unsigned>(contentLength));
  client.print("Connection: close\r\n\r\n");

  if (!writeText(client, preamble, outError)) {
    return false;
  }

  if (!writeBody(client, outError)) {
    return false;
  }

  if (!writeText(client, postamble, outError)) {
    return false;
  }

  String statusLine;
  if (!readStatusLine(client, statusLine, outError)) {
    return false;
  }

  int statusCode = 0;
  if (!parseHttpStatusCode(statusLine, statusCode)) {
    outError = "Unable to parse HTTP status line: " + statusLine;
    return false;
  }

  if (!skipHttpHeaders(client, outError)) {
    return false;
  }

  if (statusCode != 200) {
    String body = client.readString();
    body.trim();
    outError = "HTTP error: " + statusLine + " body=" + body;
    return false;
  }

  if (!parseTranscriptionBody(client, outText, outError)) {
    return false;
  }

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
  if (sampleCount > (UINT32_MAX / sizeof(int16_t))) {
    outError = "PCM sample count is too large";
    return false;
  }

  const uint32_t dataBytes = static_cast<uint32_t>(sampleCount * sizeof(int16_t));
  std::array<uint8_t, kWavHeaderSize> wavHeader{};
  buildWavHeader(wavHeader.data(), sampleRate, dataBytes);

  const String preamble = buildMultipartPreamble("live.wav");
  return executeTranscriptionRequest(
      preamble,
      sizeof(wavHeader) + dataBytes,
      [&](WiFiClientSecure &client, String &writeError) {
        if (!writeAll(client, wavHeader.data(), wavHeader.size(), writeError)) {
          return false;
        }
        return writeAll(client, reinterpret_cast<const uint8_t *>(samples), dataBytes, writeError);
      },
      outText,
      outError);
}

bool transcribeWavFromSd(const char *sdPath, String &outText, String &outError) {
  outText = "";
  outError = "";

  if (GROQ_API_KEY[0] == '\0') {
    outError = "GROQ_API_KEY is empty in secrets.h";
    return false;
  }
  if (sdPath == nullptr || sdPath[0] == '\0') {
    outError = "SD path is empty";
    return false;
  }

  File wavFile = SD.open(sdPath, FILE_READ);
  if (!wavFile) {
    outError = String("Failed to open SD file: ") + sdPath;
    return false;
  }

  ScopedFileClose wavCloser(wavFile);

  const String preamble = buildMultipartPreamble("audio.wav");
  const size_t bodyBytes = static_cast<size_t>(wavFile.size());
  return executeTranscriptionRequest(
      preamble,
      bodyBytes,
      [&](WiFiClientSecure &client, String &writeError) {
        std::array<uint8_t, kMultipartChunkSize> buffer{};
        while (wavFile.available()) {
          const size_t readBytes = wavFile.read(buffer.data(), buffer.size());
          if (readBytes == 0) {
            writeError = "Failed while reading SD WAV file";
            return false;
          }
          if (!writeAll(client, buffer.data(), readBytes, writeError)) {
            return false;
          }
        }
        return true;
      },
      outText,
      outError);
}

} // namespace GroqStt
