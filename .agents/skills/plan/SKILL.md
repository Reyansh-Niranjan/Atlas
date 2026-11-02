---
name: plan
description: Project-scoped architectural analysis and implementation planning workflow for the ATLAS project. Extends the built-in /plan command with 7 analytical specialist skills under embedded hardware constraints. Planning-only.
---

# `/plan` — ATLAS Architectural Analysis & Implementation Planning Workflow

Use this orchestrator to analyze architecture, assess feasibility, design module seams, and create comprehensive, approved implementation plans for the ATLAS firmware project (`D:\Atlas`).

> [!IMPORTANT]
> **Strictly Planning-Only:** This workflow must NEVER begin code implementation automatically. Its sole deliverable is an actionable, validated implementation plan presented as an artifact for user review and approval.

---

## 1. Project-Scoped Extension & Architecture

Antigravity defines a built-in planning capability. This workflow extends `/plan` exclusively within `D:\Atlas` using Antigravity's workspace skill discovery mechanism (`.agents/skills/plan/SKILL.md`, Priority 1).

- **Scope:** Strictly project-local to `D:\Atlas`.
- **Global Preservation:** Does not modify global Antigravity configuration (`~/.gemini/config`) or application binaries. Other workspaces continue using standard built-in planning.
- **Artifact Standard:** Retains the built-in artifact convention, producing `<appDataDir>\brain\<conversation-id>\implementation_plan.md` with `RequestFeedback: true`.

---

## 2. Skill Activation Manifest

### Exactly Activated Skills (7)
You **MUST** explicitly load and apply the following seven analytical skills before constructing a plan:
1. [`codebase-design`](file:///D:/Atlas/.agents/skills/codebase-design/SKILL.md): Designing deep modules behind narrow interfaces, defining Feathers seams, and evaluating leverage and locality.
2. [`domain-modeling`](file:///D:/Atlas/.agents/skills/domain-modeling/SKILL.md): Grounding plans in ATLAS domain concepts (`docs/ARCHITECTURE.md`), challenging vague terminology, and drafting ADRs where architectural decisions are non-obvious.
3. [`improve-codebase-architecture`](file:///D:/Atlas/.agents/skills/improve-codebase-architecture/SKILL.md): Scanning for shallow layers, boundary leaks, and structural friction before proposing changes.
4. [`karpathy-guidelines`](file:///D:/Atlas/.agents/skills/karpathy-guidelines/SKILL.md): Think before coding, simplicity first, surgical scope, and defining verifiable success criteria.
5. [`ponytail`](file:///D:/Atlas/.agents/skills/ponytail/SKILL.md): Senior developer laziness — YAGNI, standard library/ESP-IDF features first, refusing speculative complexity.
6. [`ponytail-audit`](file:///D:/Atlas/.agents/skills/ponytail-audit/SKILL.md): Pre-planning audit to prevent bloat, redundant implementations, and dead abstractions.
7. [`slopmonster`](file:///D:/Atlas/.agents/skills/slopmonster/SKILL.md): Clean, concrete planning prose free from AI fluff, marketing jargon, and hand-waving.

### Explicitly Excluded Skills
- **`diagnosing-bugs`**: DO NOT ACTIVATE. Planning is not a defect debugging loop.
- **`setup-matt-pocock-skills`**: DO NOT ACTIVATE.

---

## 3. ATLAS Embedded Architecture Context & Constraints

Every architectural proposal and plan must be validated against ATLAS hardware constraints:

- **Target Hardware:** ESP32-S3 (Xtensa Dual-Core 240MHz, 512KB SRAM, 8MB Octal PSRAM, 16MB SPI Flash).
- **Runtime Foundation:** ESP-IDF v6.0.1, FreeRTOS, PlatformIO.
- **Subsystem Map:**
  - `boot/`: Multi-stage bootloader sequence (`atlas_boot.c/.h`).
  - `core/`: Event loop (`atlas_event`), logging (`atlas_log`), service registry (`atlas_service`), hardware telemetry (`atlas_sysinfo`).
  - `drivers/`: ILI9341 LCD via SPI DMA, joystick ADC1 + GPIO buttons, I2S audio (DAC TX / Mic RX), SDSPI FatFS.
  - `system/`: Application registry/manager (`app_manager`), OTA engine (`atlas_ota`).
  - `ui/`: Framebuffer rendering (`atlas_gfx`), home shell (`screen_home`), recovery mode (`screen_recovery`).
  - `APPS/`: Git submodule application catalogue.

### Mandatory Architectural Constraints
1. **Firmware updates and application installations are separate operations:** Never conflate firmware image flashing (`atlas_ota`) with installing app packages.
2. **OTA partitions must be managed by the OTA subsystem:** Enforce dual-slot safety (`ota_0`/`ota_1`), rollback confirmation, and pre-flash image header validation.
3. **Third-party applications must not be described as executable until their runtime is implemented and verified:** Distinguish bytecode/WASM sandboxes from bare-metal execution.
4. **Manifest permissions are metadata unless actual enforcement exists:** Truthfully describe current security capabilities.
5. **APPS is a community application catalogue and source repository:** Not a firmware binary dumping ground.
6. **Never format the SD card or destroy user data.** Mount safely at `/sdcard`.
7. **Never assume hardware pin assignments:** Verify pins in `main/include/atlas_pins.h` and schematics.
8. **Distinguish successful compilation from physical hardware validation:** Include hardware validation checkpoints in every plan.
9. **Consider embedded memory, storage, CPU and scheduling constraints:** Allocate large buffers in PSRAM; reserve internal SRAM for DMA; respect FreeRTOS task stack sizes.
10. **Do not introduce desktop-OS complexity without a demonstrated requirement.**

---

## 4. Enhanced 14-Step Planning Workflow

1. **Read ATLAS Project Instructions:** Thoroughly inspect `docs/ARCHITECTURE.md`, `docs/OTA_AND_LAUNCH_AUDIT.md`, `docs/STORAGE_DESIGN.md`, and relevant subsystem headers.
2. **Load and Apply Assigned Skills:** Explicitly read the 7 assigned skill files and apply their analytical principles.
3. **Inspect the Actual Codebase:** Read relevant source files and header definitions before formulating proposals.
4. **Understand Existing Architecture:** Trace subsystem boundaries, FreeRTOS tasks, event flows, and driver interactions.
5. **Map Implementation State:** Identify fully implemented, partially implemented, and missing functionality related to the request.
6. **Detect Duplicate & Unnecessary Abstractions:** Audit existing modules using Ponytail and Codebase Design to avoid reinventing existing patterns.
7. **Analyze Architectural Friction:** Check whether the proposed change improves or compromises module depth and locality.
8. **Audit for Slop and Maintainability Concerns:** Ensure proposed interfaces are clean, minimal, and free from speculative configuration layers.
9. **Evaluate Embedded Hardware Constraints:** Verify memory footprint (SRAM vs PSRAM), DMA alignment, flash wear, and CPU load.
10. **Identify Risks, Trade-Offs, and Prerequisites:** Explicitly list affected subsystems, breaking changes, and dependencies.
11. **Develop an Actionable Implementation Plan:**
    - Break work into small, verifiable phases.
    - Specify exact files to create, modify, or delete.
    - Provide concrete interface signatures and data structures.
12. **Specify Verification Requirements:** Define automated build commands (`pio run`), regression checks, and physical hardware validation steps.
13. **Distinguish Facts from Assumptions:** Explicitly label verified facts, working assumptions, and proposed modifications.
14. **Present the Plan for User Approval:**
    - Write the plan as an artifact to `<appDataDir>\brain\<conversation-id>\implementation_plan.md` with `RequestFeedback: true`, `Summary: ...`, and `UserFacing: true`.
    - **HALT and await user feedback.** Do NOT implement any code changes until the user approves.

