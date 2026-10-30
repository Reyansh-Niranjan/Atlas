# ATLAS OS Architecture Specification

**Milestone:** 1.0 (Core Architecture Foundation)  
**Target Hardware:** ESP32-S3 (Xtensa Dual-Core 240MHz, 512KB SRAM, 8MB Octal PSRAM, 16MB Flash)  
**Underlying Runtime:** ESP-IDF v5/v6 & FreeRTOS  

---

## 1. Architectural Philosophy: Linux Concepts for Embedded Systems

ATLAS OS adapts core Linux architectural paradigms to resource-constrained microcontrollers without running a monolithic MMU kernel.

| Linux Concept | ATLAS OS Equivalent | Implementation Mechanism |
|---|---|---|
| **Kernel Subsystems** | Core Services | Modular FreeRTOS services with explicit interfaces |
| **Device Drivers** | Hardware Abstraction Layer (HAL) | ESP-IDF driver layer with non-blocking DMA pipelines |
| **Virtual Filesystem (VFS)** | Unified Storage Interface | ESP-IDF VFS wrapping FatFS (`/sdcard`) and SPIFFS (`/storage`) |
| **`/etc` (Configuration)** | System Config Service | NVS key-value store + `/sdcard/config` |
| **`/var/log` (System Logs)** | Centralized Logger | `atlas_log` wrapping `esp_log` with file sink routing |
| **`/proc` & `/sys`** | System Info API | Read-only `atlas_sysinfo` querying heap, PSRAM, tasks, uptime |
| **Init System (systemd/init)**| Structured Boot Manager | Multi-stage staged boot sequence with explicit dependencies |
| **Event System (`epoll`/`udev`)**| Inter-Service Event Bus | ESP-IDF default event loop (`esp_event`) with typed payloads |
| **Service Management** | Service Registry | Lightweight registry tracking service lifecycle & health |
| **Package Manager (`dpkg`/`apk`)**| APKG Manager | Manifest validation, dependency resolution, asset deployment |
| **Process Management** | App Lifecycle Manager | Cooperative task execution & state-machine lifecycle |
| **Desktop Environment** | ATLAS Shell | Interactive launcher, status bar, notification overlay |

---

## 2. Directory Hierarchy

```text
main/
├── boot/                   # Boot Manager & Init Stages
│   ├── atlas_boot.h
│   └── atlas_boot.c
├── core/                   # Kernel-equivalent Foundation Subsystems
│   ├── atlas_log.h         # Centralized Component Logging
│   ├── atlas_event.h/.c    # Inter-Service Event Bus
│   ├── atlas_service.h/.c  # Lightweight Service Registry
│   └── atlas_sysinfo.h/.c  # Read-Only System Diagnostics & Hardware State
├── drivers/                # Hardware Device Drivers (HAL)
│   ├── atlas_display.c/.h  # ILI9341 LCD Driver (SPI DMA)
│   ├── atlas_input.c/.h    # Joystick & Button Debounce Task
│   ├── atlas_audio.c/.h    # MAX98357A & INMP441 I2S Driver
│   └── atlas_sdcard.c/.h   # SDSPI FatFS Driver
├── system/                 # System Management & App Registry
│   ├── app_registry.c/.h   # App Discovery & In-Firmware Dispatch
│   └── atlas_init.c/.h     # Compatibility Layer
├── ui/                     # ATLAS Shell & User Interface
│   ├── atlas_gfx.c/.h      # Framebuffer Graphics Primitives
│   ├── screen_home.c/.h    # 2x2 Grid Home Launcher Shell
│   └── screen_recovery.c/.h# Recovery Safe Mode UI
└── main.c                  # System Entry Point
```

---

## 3. Core Foundations (Milestone 1)

### 3.1 Structured Boot Manager (`boot/atlas_boot`)
The boot manager eliminates monolithic initialization routines by breaking the boot pipeline into explicit stages:
1. **`ATLAS_BOOT_STAGE_EARLY`:** Initialize NVS, core event loop, and service registry.
2. **`ATLAS_BOOT_STAGE_HARDWARE`:** Initialize PSRAM, ILI9341 display, allocate boot framebuffer.
3. **`ATLAS_BOOT_STAGE_STORAGE`:** Mount MicroSD FatFS at `/sdcard`. Emit `ATLAS_STORAGE_EVENT_MOUNTED`.
4. **`ATLAS_BOOT_STAGE_PERIPHERALS`:** Initialize input (ADC1 + GPIO debouncer), audio I2S pipelines, allow 100ms settling time.
5. **`ATLAS_BOOT_STAGE_DIAGNOSTICS`:** Run hardware self-tests (PSRAM, display, SD, input, audio, mic RX).
6. **`ATLAS_BOOT_STAGE_RECOVERY`:** Check Button B held state; if held, invoke Recovery Safe Mode.
7. **`ATLAS_BOOT_STAGE_SERVICES`:** Initialize application registry, enforce boot partition safety, index applications.
8. **`ATLAS_BOOT_STAGE_SHELL`:** Hand off execution to the interactive Home Launcher Shell.

### 3.2 Inter-Service Event Bus (`core/atlas_event`)
Built directly on ESP-IDF's lightweight `esp_event` facility:
- **Zero dynamic memory overhead:** Event dispatches utilize stack payloads copied directly into the event loop queue.
- **Typed Event Families:**
  - `ATLAS_EVENT_SYSTEM`: Boot start, stage advance, boot completed, low memory, shutdown, reboot.
  - `ATLAS_EVENT_STORAGE`: Media mounted, media unmounted, storage error.
  - `ATLAS_EVENT_INPUT`: Button click, long press, joystick gesture, state change.
  - `ATLAS_EVENT_SERVICE`: Service registered, service started, service failure.
- **Decoupled Architecture:** Subsystems subscribe to events without direct compile-time coupling to emitters.

### 3.3 Centralized Logging (`core/atlas_log`)
Wraps ESP-IDF's `esp_log` system with unified tags, consistent severity levels (`ERROR`, `WARN`, `INFO`, `DEBUG`), and standardized formatting:
- Prefixing standard component identifiers (`[SYS]`, `[DRV]`, `[BOOT]`, `[APP]`, `[FS]`).
- Zero performance overhead in release builds via compiler dead-code elimination and log-level filtering.

### 3.4 Lightweight Service Registry (`core/atlas_service`)
Provides a registry of system services:
- **Service Descriptor:** Unique ID, human-readable name, priority, state (`UNREGISTERED`, `INITIALIZED`, `RUNNING`, `FAILED`, `STOPPED`).
- **Dependency Tracking:** Services can specify required prerequisite services before starting.
- **Observability:** Queries expose service operational status for UI diagnostics.

### 3.5 System Information API (`core/atlas_sysinfo`)
Read-only truthful hardware and kernel telemetry:
- ESP32-S3 silicon revision, CPU frequency (240MHz), core count (2).
- Free heap & minimum free heap across internal SRAM and external PSRAM.
- System uptime in microseconds and formatted seconds.
- Storage capacity and free space for `/sdcard` and flash partitions.
- Boot diagnostic snapshot.

---

## 4. Application Architecture & Future Package Design

### 4.1 Separation of Concerns
```text
┌─────────────────────────────────────────────────────────────┐
│                    Dynamic Launcher (Shell)                 │
└──────────────────────────────┬──────────────────────────────┘
                               │ Queries applications
┌──────────────────────────────▼──────────────────────────────┐
│                    Application Manager                      │
├──────────────────────────────┬──────────────────────────────┤
│ Queries packages             │ Dispatches execution         │
┌▼─────────────────────────────▼┐ ┌───────────────────────────▼┐
│        Package Manager        │ │    Application Runtime     │
│   (APKG Archive & Manifest)   │ │  (In-Firmware / Bytecode)  │
└───────────────────────────────┘ └────────────────────────────┘
```

1. **Launcher Shell:** Renders UI cards. It never interacts with the SD card driver directly.
2. **Application Manager:** Validates whether an application has a compatible runtime before displaying it as launchable.
3. **Package Manager:** Handles discovery, signature verification, installation, and asset decompression of `.apkg` bundles.
4. **Application Runtime:** Manages the execution environment and sandbox boundaries.

### 4.2 Runtime Alternatives Comparison

| Evaluation Criteria | 1. Native Compiled (In-Firmware) | 2. Dynamic Native Modules (ELF/PIC) | 3. Bytecode Virtual Machine (e.g. WASM / MicroPython) |
|---|---|---|---|
| **RAM Footprint** | Minimal (Zero extra engine overhead) | Moderate (Relocation tables & dynamic symbols) | High (VM state + interpreter memory: ~60-120KB) |
| **PSRAM Utilization** | Low | Low to Moderate | High (Ideal fit for 8MB PSRAM) |
| **Startup Latency** | Instant (0 ms) | Fast (< 5 ms) | Moderate (10-50 ms parsing & JIT/bytecode init) |
| **Execution Speed** | Maximum (100% native 240MHz Xtensa) | Maximum (100% native Xtensa) | 5x - 20x slower (interpretive loop) |
| **Process Isolation** | None (Single address space; crash halts device)| None (Bad pointer corrupts OS kernel) | Strong (Memory sandbox, boundary checks, crash recovery) |
| **Security & Safety** | Trusted code only | Dangerous for 3rd party code | Excellent (Hard limits on memory & peripheral access) |
| **API Complexity** | Zero (Direct C function calls) | Very High (Requires dynamic symbol linker on ESP32) | Moderate (Exported syscall table to VM) |
| **Developer Experience**| C/C++ firmware rebuild required | Complex toolchain (PIC flags, custom linker scripts) | Excellent (High-level script or compiled WASM bytecodes) |
| **Maintainability** | Simple & stable | Fragile across ESP-IDF versions | High (Virtual architecture decoupled from silicon) |

**Architectural Recommendation:**
- **System Apps (Core UI, Settings, File Browser, Media Player):** Native compiled in-firmware.
- **Third-Party Extensible Apps:** WebAssembly (WASM3) or Lightweight Bytecode VM executing out of 8MB PSRAM.

---

## 5. APKG Package Format Specification

The proposed ATLAS Package format (`.apkg`) is a standardized ZIP/TAR container containing:

```text
app.apkg
├── manifest.json       # Canonical package metadata
├── icon.raw            # 32x32 or 48x48 RGB565 icon
├── app.wasm / main.py  # Application executable or bytecode
└── assets/             # Images, sounds, level data
    └── font.bin
```

### 5.1 Manifest Specification (`manifest.json`)
```json
{
  "apkg_version": 1,
  "app_id": "org.atlas.calc",
  "name": "Scientific Calculator",
  "version": "1.0.2",
  "developer": "Reyansh",
  "target_atlas_version": ">=1.0.0",
  "runtime": "wasm",
  "entrypoint": "app.wasm",
  "memory_limit_kb": 256,
  "permissions": [
    "display",
    "input",
    "audio_tx",
    "storage_app_data"
  ],
  "checksum_sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
}
```

