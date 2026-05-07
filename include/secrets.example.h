#pragma once

// Local Wi-Fi used by the device before OTA checks.
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Hardcoded SafeGithubOTA settings (no provisioning portal).
#define OTA_GITHUB_OWNER "YOUR_GITHUB_OWNER"
#define OTA_GITHUB_REPO "YOUR_GITHUB_REPO"
#define OTA_BIN_FILENAME "firmware.bin"

// Groq STT settings.
// Set GROQ_API_KEY before using Groq speech-to-text.
#define GROQ_API_KEY ""
#define GROQ_STT_MODEL "whisper-large-v3-turbo"
#define GROQ_STT_LANGUAGE "en"
