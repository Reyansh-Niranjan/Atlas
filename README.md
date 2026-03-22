This is MY APPROACH TO A DIY smarthphone.
With a little bit of beginner juice, geek for geeks and youtube this is made possible!

Project STATUS:
    V3 WORK in PROGRESS!
    SD and TFT Completed!

## GitHub OTA (SafeGithubOTA library)

This project uses `gibz104/SafeGithubOTA` for OTA updates from GitHub Releases.

### 1) Set Wi-Fi credentials

Edit `include/secrets.h`:

- `WIFI_SSID`
- `WIFI_PASSWORD`

### 2) Build and upload once over USB

```bash
pio run -t upload
```

### 3) Provision GitHub OTA on first boot

If OTA is not configured yet, the device starts a captive portal AP:

- SSID: `EduScraper-OTA`

Enter these values in the portal form:

- GitHub owner
- GitHub repo
- GitHub token (PAT with read access)
- Firmware binary asset name (example: `firmware.bin`)

### 4) Publish updates

1. Update `FW_VERSION` in `platformio.ini`.
2. Build firmware with `pio run`.
3. Upload `.pio/build/4d_systems_esp32s3_gen4_r8n16/firmware.bin` to a new GitHub Release.
4. Tag the release as semantic version (example: `1.0.1`).

On boot, and every 6 hours in runtime, the firmware checks for a newer GitHub release and updates automatically.

## Automated firmware.bin creation

A GitHub Actions workflow is included at .github/workflows/build-firmware.yml.

PlatformIO also runs scripts/patch_safegithubota.py as a pre-build step (configured in platformio.ini via extra_scripts), so SafeGithubOTA is auto-patched during build according to project needs.

It will:

- Build firmware for environment 4d_systems_esp32s3_gen4_r8n16
- Export dist/firmware.bin
- Upload firmware.bin as a workflow artifact named firmware-bin
- Automatically create/update a GitHub Release on version tag push and upload firmware.bin

How to run:

1. Push changes to main, or open a pull request.
2. Create and push a tag (example: v0.0.2) to auto-create a release with firmware.bin.
3. Or run it manually from Actions and set tag_name to create/upload a release.
4. Download firmware-bin from workflow artifacts if needed.