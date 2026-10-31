# ATLAS OS — Firmware Launch Architecture & OTA Partition Safety Audit

**Date:** October 2026  
**Audited Target:** Commit `8df30ff898b9a040b10e0353375e89d0c0e59b7f` through latest release  
**Architecture:** ESP32-S3 (Xtensa LX7 Dual-Core, 16MB Flash, 8MB Octal PSRAM)  
**Framework:** ESP-IDF v6.0.1 / PlatformIO  

---

## 1. Executive Summary

This audit assesses the Application Registry, Firmware Staging, OTA Partition Management, and Boot Recovery mechanisms introduced in commit `8df30ff` and refactored across Milestones 1 and 2.

The central finding of this investigation is that **the staging mechanism in commit `8df30ff` was not an application launcher; it was an unconstrained OTA firmware replacement engine masquerading as an application manager**. 

Flashing raw `.bin` files into flash partition `ota_1` and issuing `esp_restart()` completely replaces the running operating system, destroys the host OS execution context, lacks memory safety and process isolation, and risks permanent bootloops if the target image is corrupt.

This audit documents the hazards identified in `8df30ff`, details the architectural corrections implemented, and provides the production-grade foundation for safe OTA firmware upgrades and dynamic application lifecycle management in ATLAS OS.

---

## 2. Architectural Analysis: Firmware Replacement vs. Application Launching

### 2.1 The Reality of Microcontroller Process Models

In traditional desktop and server operating systems (e.g., Linux), the kernel uses a Memory Management Unit (MMU) to enforce virtual address spaces. An application is an ELF binary loaded into an isolated user-space process (`fork()` + `execve()`). The kernel schedules the process, enforces page permissions, intercepts system calls, and handles crashes (`SIGSEGV`) without halting the OS.

The ESP32-S3 microcontroller features a memory cache and MMU for external flash/PSRAM mapping, but **it does not support an MMU-enforced virtual memory user/kernel process separation**:
- All code runs in a single physical/virtual address space.
- An ESP-IDF firmware binary (`.bin`) is a **monolithic system image**: it contains second-stage bootloader headers, FreeRTOS kernel routines, interrupt vector tables, peripheral hardware drivers, and the application logic.

### 2.2 Why Staging to `ota_1` is NOT Application Execution

In commit `8df30ff`, `app_registry_stage_and_launch()` implemented the following sequence:
1. Open an arbitrary `.bin` from `/sdcard/apps/<app_id>.bin`.
2. Write the binary into flash partition `ota_1` using `esp_ota_begin()` / `esp_ota_write()` / `esp_ota_end()`.
3. Set `ota_1` as the boot target using `esp_ota_set_boot_partition()`.
4. Call `esp_restart()`.

This architecture suffered from four fundamental flaws:
1. **Total Context Obliteration:** When `esp_restart()` is invoked, the CPU reboots. SRAM and PSRAM are reinitialized. ATLAS OS, its event bus, service registry, drivers, and UI are completely destroyed. ATLAS OS is no longer running.
2. **Absence of Host Control & Sandboxing:** The guest image assumes 100% control over the ESP32-S3 silicon. If the image panics, deadlocks, or disables interrupts, the host OS cannot intercept or recover.
3. **No Return Path:** Once booted into the new image, there is no mechanism to "exit" back to ATLAS OS unless the guest image is specifically modified to understand ATLAS partition tables and manipulate `otadata`.
4. **Flash Wear & Startup Latency:** Writing 2MB–4MB to SPI NOR flash takes 5 to 25 seconds and consumes flash write endurance cycles. Treating this as an application launch is unsustainable.

### 2.3 The Honest ATLAS OS Model

ATLAS OS strictly separates three concepts:

| Category | Mechanism | Execution Domain | Lifecycle |
|---|---|---|---|
| **Built-in System Applications** | In-Firmware C Handlers | Compiled into running firmware | Cooperative dispatch via `app_manager_launch()` |
| **Installed User Applications** | Sandboxed Bytecode / WASM | Executed inside a VM in 8MB PSRAM | Isolated memory sandbox; crash does not halt OS |
| **Firmware Upgrades (OTA)** | Dedicated `atlas_ota` Engine | Flashed to inactive OTA slot | Guarded by hardware rollback and user confirmation |

---

## 3. Critical Audit Findings in Commit `8df30ff`

| Vulnerability / Defect | Severity | Architectural Impact | Correction |
|---|---|---|---|
| **Hardcoded `ota_1` Target** | **CRITICAL** | If ATLAS OS was running on `ota_1` (e.g. after an OTA update), `app_registry_stage_and_launch()` would overwrite the currently executing partition. | Replaced with dynamic resolution via `esp_ota_get_next_update_partition(NULL)` with hard assertions (`target != running`). |
| **Missing Image Header Validation** | **HIGH** | Arbitrary files could be written to flash. Images for other silicon (ESP32, ESP32-C3) or corrupted files were flashed before failure detection. | Pre-flash check validates magic byte `0xE9` and ESP32-S3 chip ID (`0x0009`) before touching flash. |
| **Disabled Rollback Support** | **HIGH** | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` was not enabled. Booting a crashing image created an unrecoverable bootloop. | Enabled `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. Implemented `atlas_ota_validate_boot()`. |
| **Blind Boot Forcing in `ensure_boot_os`** | **HIGH** | Forced boot partition to `ota_0` even when running from `factory`. If `ota_0` was blank (0xFF), bootloader crashed on boot. | Resolved running partition truthfully; only verified OTA partitions are marked bootable. |
| **Conflicting Registries** | **MEDIUM** | `app_registry` indexed `.bin` files in `/sdcard/apps/`, while `app_manager` indexed manifest packages in `/sdcard/apps/installed/`. | Consolidated `app_manager` as single source of truth; deprecated raw `.bin` scanning. |
| **Incomplete & Unsafe Uninstall** | **MEDIUM** | `app_registry_uninstall` only unlinked `/sdcard/apps/%s.bin`, leaving directory bundles intact and failing to protect built-in apps. | Implemented `app_manager_uninstall()`: protects built-in apps, prevents path traversal, and recursively cleans directories. |

---

## 4. Safety Architecture Implementation

### 4.1 Flash Partition Layout (`partitions.csv`)

```text
0x000000 ┌────────────────────────────────────────┐
         │ Bootloader (0x0000 - 0x9000)           │
0x009000 ├────────────────────────────────────────┤
         │ NVS (64 KB)                            │
0x019000 ├────────────────────────────────────────┤
         │ otadata (8 KB, Dual Slot States)       │
0x01B000 ├────────────────────────────────────────┤
         │ phy_init (4 KB)                        │
0x020000 ├────────────────────────────────────────┤
         │ factory (2.5 MB, Factory Recovery OS)  │
0x2A0000 ├────────────────────────────────────────┤
         │ ota_0 (4 MB, Primary ATLAS OS Slot)    │
0x6A0000 ├────────────────────────────────────────┤
         │ ota_1 (4 MB, Secondary ATLAS OS Slot)  │
0xAA0000 ├────────────────────────────────────────┤
         │ storage (5.375 MB, SPIFFS Data Store)  │
0x1000000└────────────────────────────────────────┘ (16 MB SPI Flash)
```

### 4.2 Dedicated OTA Engine (`system/atlas_ota.c/.h`)

The firmware update capability is completely decoupled from application management into `atlas_ota`:

1. **Pre-flash Verification:**
   - Validates file existence and regular file status via `stat()`.
   - Inspects `esp_image_header_t`: verifies `magic == 0xE9` and `chip_id == ESP_CHIP_ID_ESP32S3`.
   - Rejects files larger than the target partition size.
2. **Safe Slot Resolution:**
   - Queries `running = esp_ota_get_running_partition()`.
   - Queries `target = esp_ota_get_next_update_partition(NULL)`.
   - Asserts `target != running`. The running partition can **never** be overwritten.
3. **Atomic Streaming & Abort:**
   - Streams in 4 KB chunks.
   - Any I/O or flash write error immediately triggers `esp_ota_abort(ota_handle)`.
   - The boot partition is **never altered** if any error occurs during streaming.
4. **Cryptographic Integrity & Rollback Commitment:**
   - `esp_ota_end(ota_handle)` validates the SHA-256 digest and image checksum.
   - `esp_ota_set_boot_partition(target)` commits the boot target only after validation.
   - Newly flashed partitions boot in `ESP_OTA_IMG_PENDING_VERIFY`.

### 4.3 Hardware Rollback & Boot Health (`atlas_ota_validate_boot`)

During Stage 7 of system boot (`boot_stage_services` in `atlas_boot.c`):
- `atlas_ota_validate_boot()` queries the running partition state.
- If state is `ESP_OTA_IMG_PENDING_VERIFY`, the system confirms boot diagnostics passed and calls `esp_ota_mark_app_valid_cancel_rollback()`.
- If an upgraded firmware crashes before completing boot diagnostics, the ESP-IDF hardware bootloader automatically reverts to the previous valid partition on the next restart.

### 4.4 Recovery Safe Mode Enhancement (`screen_recovery.c`)

Recovery Safe Mode (triggered by holding Button B / SW2 at boot):
- Displays the active partition label (`factory`, `ota_0`, `ota_1`) and physical flash address.
- Queries `esp_ota_check_rollback_is_possible()`.
- If a valid fallback partition exists, pressing **Button B** triggers `atlas_ota_rollback()`, allowing instant recovery from bad firmware updates directly from the hardware controls.

### 4.5 Application Manager & Package Uninstallation (`app_manager.c/.h`)

`app_manager` acts as the single source of truth:
- **Built-in Protection:** Built-in apps (`settings`) cannot be uninstalled (`ESP_ERR_NOT_SUPPORTED`).
- **Security:** Package uninstallation validates `app_id` against directory traversal (`..`, `/`, `\`).
- **Filesystem Cleanup:** `rm_dir_recursive()` safely removes all files and subdirectories within `/sdcard/apps/installed/<app_id>/`.
- **Catalogue Sync:** Updates the in-memory registry and emits `ATLAS_EVENT_APP_CATALOGUE_CHANGED`.

---

## 5. Verification & Test Matrix

| Test Scope | Tool / Method | Results |
|---|---|---|
| **Compilation & Linking** | `pio run` (ESP-IDF v6.0.1, GCC 15.2.0) | **SUCCESS** (RAM: 7.0%, Flash: 14.3%) |
| **Bootloader Rollback Integration** | PlatformIO Ninja Build | **SUCCESS** (`bootloader.elf` with rollback compiled) |
| **Static Code Analysis** | `pio check` (cppcheck) | **PASSED** (0 HIGH, 0 MEDIUM defects) |
| **Manifest & Package Validation** | `scripts/test_manifest_validation.py` | **14 / 14 TEST CASES PASSED** |

---

## 6. Conclusion & Recommendations

1. **Firmware updates are now strictly quarantined** within `atlas_ota`, protected by pre-flash checks, non-running slot selection, and hardware rollback.
2. **Application management is truthful and dynamic**: only real built-in applications are registered, third-party packages are validated against `manifest.json`, and unlaunchable runtimes are honestly flagged.
3. **Third-party application execution** should proceed with a WebAssembly runtime (e.g. WAMR or WASM3) executing within the 8MB Octal PSRAM boundary, providing true memory sandboxing without firmware flashing.
