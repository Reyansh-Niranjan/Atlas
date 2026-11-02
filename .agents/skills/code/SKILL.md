---
name: code
description: Orchestrated engineering workflow for general ATLAS firmware development and implementation. Activates codebase-design, domain-modeling, improve-codebase-architecture, karpathy-guidelines, ponytail, ponytail-audit, and slopmonster under embedded hardware constraints.
---

# `/code` — ATLAS Engineering Implementation Workflow

Use this orchestrator for general development, feature implementation, and code modifications in the ATLAS project (`D:\Atlas`).

This workflow coordinates exactly seven specialist skills to produce disciplined, minimal, and hardware-verified embedded code.

---

## 1. Skill Activation Manifest

### Exactly Activated Skills (7)
You **MUST** load and apply the following seven specialist skills before implementing changes. Inspect their instructions at their project-local paths:
1. [`codebase-design`](file:///D:/Atlas/.agents/skills/codebase-design/SKILL.md): Deep modules (small interface, substantial implementation), Michael Feathers seams, testable leverage, and locality.
2. [`domain-modeling`](file:///D:/Atlas/.agents/skills/domain-modeling/SKILL.md): Precise domain language, architectural boundary definitions, and alignment with architecture specifications.
3. [`improve-codebase-architecture`](file:///D:/Atlas/.agents/skills/improve-codebase-architecture/SKILL.md): Detection and elimination of shallow wrappers, boundary leaks, and architectural friction.
4. [`karpathy-guidelines`](file:///D:/Atlas/.agents/skills/karpathy-guidelines/SKILL.md): Think before coding, simplicity first, surgical changes (touch only what is necessary), and verifiable success criteria.
5. [`ponytail`](file:///D:/Atlas/.agents/skills/ponytail/SKILL.md): Senior developer laziness (YAGNI, standard library and ESP-IDF native features first, minimal code, avoid speculative abstractions).
6. [`ponytail-audit`](file:///D:/Atlas/.agents/skills/ponytail-audit/SKILL.md): Diff and module scanning for bloat, dead code, single-caller abstractions, and complexity creep.
7. [`slopmonster`](file:///D:/Atlas/.agents/skills/slopmonster/SKILL.md): Elimination of AI boilerplate, excessive commentary, defensive over-engineering, and generated-code slop.

### Explicitly Excluded Skills
- **`diagnosing-bugs`**: DO NOT ACTIVATE. Bug diagnosis belongs exclusively to `/debug`.
- **`setup-matt-pocock-skills`**: DO NOT ACTIVATE. Repository setup is dormant.

---

## 2. ATLAS Embedded Architecture Context & Constraints

Every code change must adhere to the physical realities of the ATLAS hardware platform:

- **Target Silicon:** ESP32-S3 (Xtensa Dual-Core 240MHz, 512KB SRAM, 8MB Octal PSRAM, 16MB SPI Flash).
- **Runtime Stack:** ESP-IDF v6.0.1, FreeRTOS (1000Hz tick rate), PlatformIO build environment.
- **Subsystems:**
  - `boot/`: Staged boot manager (`atlas_boot.c/.h`).
  - `core/`: Event bus (`atlas_event.c/.h`), centralized logger (`atlas_log.h`), service registry (`atlas_service.c/.h`), sysinfo (`atlas_sysinfo.c/.h`).
  - `drivers/`: ILI9341 LCD via SPI DMA (`atlas_display`), ADC1 + GPIO joystick/buttons (`atlas_input`), MAX98357A & INMP441 I2S audio (`atlas_audio`), SDSPI FatFS (`atlas_sdcard`).
  - `system/`: Application manager (`app_manager.c/.h`), OTA update engine (`atlas_ota.c/.h`), partition safety.
  - `ui/`: Framebuffer graphics (`atlas_gfx.c/.h`), home launcher (`screen_home.c/.h`), recovery UI (`screen_recovery.c/.h`).
  - `APPS/`: Community application repository integrated as a Git submodule.

### Mandatory Architectural Constraints
1. **Firmware updates and application installations are separate operations:** Never conflate flashing an OS image with installing an application package.
2. **OTA partitions must be managed by the OTA subsystem:** Never stage or write raw binaries to active or unverified OTA partitions directly from application logic.
3. **Third-party applications must not be described as executable until their runtime is implemented and verified:** Never claim an unexecuted bytecode format or binary runs natively on bare metal without an active, verified sandbox.
4. **Manifest permissions are metadata unless actual enforcement exists:** Document truthful security posture.
5. **APPS is a community application catalogue and source repository:** It is not a dumping ground for arbitrary firmware binaries.
6. **Never format the SD card or destroy user data.** Mount safely at `/sdcard`.
7. **Never assume hardware pin assignments:** Always verify against `main/include/atlas_pins.h` and hardware schematics.
8. **Distinguish successful compilation from physical hardware validation:** Clean compilation is a prerequisite, not proof of peripheral timing or DMA stability.
9. **Consider embedded constraints:** Internal SRAM is scarce (512KB); use 8MB PSRAM for large framebuffers and allocations via `MALLOC_CAP_SPIRAM`. Respect FreeRTOS task stack budgets and priority inversion rules.
10. **Do not introduce desktop-OS complexity:** No dynamic MMU processes, unnecessary thread pools, or monolithic abstractions without demonstrated embedded need.

---

## 3. Step-by-Step `/code` Execution Workflow

When `/code` is triggered:

1. **Read ATLAS Project Instructions:** Inspect `docs/ARCHITECTURE.md`, `docs/OTA_AND_LAUNCH_AUDIT.md`, and relevant subsystem headers.
2. **Load and Apply Assigned Skills:** Explicitly read the 7 assigned skill files and internalize their constraints.
3. **Inspect Existing Implementation:** Read the exact files to be modified before proposing or writing code.
4. **Understand Subsystem Boundaries:** Identify affected FreeRTOS tasks, event dispatches, memory domains, and driver interfaces.
5. **Follow an Approved Plan:** If an approved implementation plan exists in the conversation, strictly follow its scope and order.
6. **Establish Scope (If No Plan Exists):** Define the exact minimal change required. Do not expand scope unilaterally.
7. **Implement Focused, Incremental Changes:**
   - Apply the **Ponytail ladder**: YAGNI → reuse existing codebase helper → stdlib/ESP-IDF feature → minimal code.
   - Apply **Karpathy surgical editing**: change only what is required; do not reformat adjacent lines or touch working code.
   - Apply **Codebase Design**: keep interfaces narrow and modules deep; avoid shallow pass-through functions.
8. **Avoid Speculative Abstractions:** Do not create generic layers, extra configuration flags, or unused extensibility.
9. **Validate Every Change:** Ensure memory allocations check for NULL, verify DMA buffers are in DMA-capable memory, and validate FreeRTOS task parameters.
10. **Run Builds and Verification:** Execute PlatformIO build (`pio run`) to confirm clean compilation and zero warnings.
11. **Audit Final Changes (Ponytail-Audit & SlopMonster):**
    - Scan diff for dead code, orphan headers, single-caller wrappers, and unnecessary comments.
    - Remove AI slop (e.g. "comprehensive", "seamless", over-commented obvious code).
12. **Report Engineering Results:**
    - Modified/created files with clickable paths.
    - Architectural decisions and trade-offs made.
    - Compilation/test results.
    - Remaining limitations and physical validation notes.

---

## 4. Conflict Resolution Rules

If instructions or guidelines appear in tension:
1. Explicit user instructions take first priority.
2. ATLAS embedded safety constraints take precedence over generic refactoring.
3. Correctness and memory safety take precedence over brevity.
4. Ponytail/Karpathy simplicity takes precedence over speculative architecture deepening.
5. Report and explain any unresolved conflict rather than guessing.

