# ATLAS OS Storage Architecture & Filesystem Design

**Document:** `docs/STORAGE_DESIGN.md`  
**Status:** Approved Architectural Specification  
**Target Platform:** ESP32-S3 (ESP-IDF VFS, FatFS, SPIFFS, NVS)  

---

## 1. Storage Overview & Physical Media

ATLAS OS manages three distinct storage layers to balance speed, endurance, non-volatility, and capacity:

```text
┌────────────────────────────────────────────────────────────────────────┐
│                          ATLAS OS Storage Layers                       │
├───────────────────┬──────────────────────────┬─────────────────────────┤
│ Media             │ Mount / Access API       │ Primary Ownership       │
├───────────────────┼──────────────────────────┼─────────────────────────┤
│ 1. NVS (64 KB)    │ `nvs_flash` API          │ Critical configuration, │
│                   │                          │ WiFi credentials, state │
├───────────────────┼──────────────────────────┼─────────────────────────┤
│ 2. SPIFFS (5.3MB) │ `/storage` (VFS SPIFFS)  │ Internal system files,  │
│                   │                          │ fallback recovery data  │
├───────────────────┼──────────────────────────┼─────────────────────────┤
│ 3. MicroSD (32GB) │ `/sdcard` (VFS FatFS)    │ Apps, packages, assets, │
│                   │                          │ logs, cache, user data  │
└───────────────────┴──────────────────────────┴─────────────────────────┘
```

---

## 2. Logical Directory Hierarchy

To provide a consistent, POSIX-compatible namespace across internal and external media, ATLAS establishes a standardized directory hierarchy:

```text
/ (Logical Root)
├── system/
│   ├── firmware/       # Golden and pending OTA firmware binaries
│   └── recovery/       # Recovery snapshots and fallback configuration
├── apps/
│   └── installed/      # Installed application bundles and metadata
├── packages/
│   └── downloads/      # Incoming APKG package archives
├── data/               # Persistent application database and save states
├── config/             # System and service configuration (.ini, .json)
├── cache/              # Ephemeral run-time cache and temporary files
└── logs/               # Rotating system and kernel logs
```

---

## 3. Storage Media Ownership Matrix

| Logical Path | Primary Medium | Secondary / Fallback Medium | Justification |
|---|---|---|---|
| `/config` | **NVS** (active) + **SD Card** (`/sdcard/config`) | SPIFFS (`/storage/config`) | NVS handles atomicity and small key-value pairs without wear. Larger config files live on SD or SPIFFS. |
| `/system/firmware` | **SD Card** (`/sdcard/system/firmware`) | Flash `ota_0` / `ota_1` | 4MB firmware update binaries require bulk storage before staging into flash OTA slots. |
| `/system/recovery` | **Flash** `factory` partition + **SD Card** | SPIFFS | High reliability recovery path must exist even if SD card is removed. |
| `/apps/installed` | **SD Card** (`/sdcard/apps/installed`) | SPIFFS (core built-ins) | Extensible application storage requires the capacity of the SD card. |
| `/packages/downloads`| **SD Card** (`/sdcard/packages/downloads`) | None | Transient downloads require high write endurance and large capacity. |
| `/data` | **SD Card** (`/sdcard/data`) | SPIFFS | User documents, game states, audio assets. |
| `/cache` | **SD Card** (`/sdcard/cache`) | RAM (tmpfs) | Prevent flash write fatigue; cache files can be safely invalidated. |
| `/logs` | **SD Card** (`/sdcard/logs`) | RAM Ring-Buffer | High write frequency; writing logs to internal flash would quickly exhaust NOR flash erase cycles. |

---

## 4. Hardware & Filesystem Evaluation

### 4.1 MicroSD Card (SDSPI over SPI2_HOST)
- **Controller:** Shared SPI2 (FSPI) with ILI9341 display.
- **Clock:** 20 MHz stable SPI bus frequency.
- **Filesystem:** FAT32 with Long File Names (LFN enabled on heap, 255 chars max).
- **Physical SD Card Inspected (Windows `E:\`):**
  - **Disk ID:** Disk 1 ("Generic- SD/MMC/MS PRO", USB Mass Storage)
  - **Volume Label:** `MSD`
  - **Capacity:** 30,979,129,344 bytes (~30.98 GB)
  - **Filesystem:** FAT32 (Allocation unit: 32 KB)
  - **Free Space:** ~28.34 GB free.
  - **Existing Data:** User folders `Class1`..`Class12` and `assets/` (preserved intact).

### 4.2 Internal Flash SPIFFS Partition
- **Size:** 5.375 MB (`0x560000` bytes) at offset `0xAA0000`.
- **Characteristics:** Wear-levelled, read-optimized, flat namespace (no true directories).
- **Recommendation:** Mount SPIFFS on demand as `/spiffs` for system fallback assets. Do not use SPIFFS for high-write logging or large multi-megabyte app packages.

### 4.3 NVS Partition
- **Size:** 64 KB (`0x10000` bytes) at offset `0x009000`.
- **Usage:** Key-value store for calibrated ADC values, volume level, active WiFi credentials, and installed app index (`atlas_apps`).

---

## 5. Proposed Storage Service Interface

ATLAS provides a unified VFS storage manager to insulate applications from direct driver interaction:

```c
/**
 * @file atlas_storage.h
 * @brief ATLAS OS VFS Storage Subsystem
 */

typedef enum {
    ATLAS_STORAGE_MEDIA_SDCARD = 0,
    ATLAS_STORAGE_MEDIA_SPIFFS,
    ATLAS_STORAGE_MEDIA_NVS,
    ATLAS_STORAGE_MEDIA_MAX
} atlas_storage_media_t;

typedef struct {
    bool is_mounted;
    uint64_t total_bytes;
    uint64_t free_bytes;
    const char *vfs_mount_point;
} atlas_storage_info_t;

/** Initialize and mount primary storage subsystems */
esp_err_t atlas_storage_init(void);

/** Query status of specific storage media */
esp_err_t atlas_storage_get_info(atlas_storage_media_t media, atlas_storage_info_t *out_info);

/** Resolve a logical path (e.g. "/apps/installed") to the physical VFS path */
esp_err_t atlas_storage_resolve_path(const char *logical_path, char *out_vfs_path, size_t max_len);
```

---

## 6. Migration Strategy

1. **Step 1:** Establish standardized directory structure on the SD card (done on `E:\`).
2. **Step 2:** Ensure firmware checks and creates missing standard directories on mount (`/sdcard/system`, `/sdcard/apps/installed`, `/sdcard/packages/downloads`, `/sdcard/data`, `/sdcard/config`, `/sdcard/cache`, `/sdcard/logs`).
3. **Step 3:** Maintain backwards-compatibility for existing `/sdcard/apps` path by alias or transparent path resolution.
4. **Step 4:** Provide future optional mount of the 5.375MB SPIFFS partition for offline system assets.

