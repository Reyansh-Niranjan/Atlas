# ATLAS Project Governance & Agent Rules

**Repository:** `D:\Atlas`  
**Target Platform:** ESP32-S3 (Xtensa LX7 Dual-Core 240MHz, 512KB SRAM, 8MB Octal PSRAM, 16MB SPI Flash)  
**Framework:** ESP-IDF v6.0.1, FreeRTOS (1000Hz), PlatformIO  

These rules apply to all agent operations within this repository. They take precedence over default or generic behaviors.

---

## 1. Native `/plan` & Plan Mode Governance

When the user invokes the native `/plan` slash command, switches to Plan mode, or requests architectural planning:

### Strict Planning-Only Constraint
- **NEVER begin implementation automatically.** Do not write or edit source code files during a planning phase.
- The sole deliverable is an actionable, validated implementation plan presented as an `implementation_plan.md` artifact in the conversation artifact directory (`<appDataDir>\brain\<conversation-id>\implementation_plan.md`) with `RequestFeedback: true`, `Summary: ...`, and `UserFacing: true`.
- **Halt and await explicit user approval** before transitioning to implementation.

### Analytical Skill Orchestration (8 Skills)
During planning and architectural evaluation, you **MUST** explicitly load and apply the following eight project-installed specialist skills by reading their instructions:
1. [`codebase-design`](file:///D:/Atlas/.agents/skills/codebase-design/SKILL.md): Design deep modules (narrow interface, rich implementation), Michael Feathers seams, testable leverage, and locality.
2. [`domain-modeling`](file:///D:/Atlas/.agents/skills/domain-modeling/SKILL.md): Ground plans in ATLAS domain concepts (`docs/ARCHITECTURE.md`), challenge fuzzy terminology, and draft ADRs where decisions are non-obvious.
3. [`improve-codebase-architecture`](file:///D:/Atlas/.agents/skills/improve-codebase-architecture/SKILL.md): Scan for shallow layers, boundary leaks, and structural friction before proposing changes.
4. [`karpathy-guidelines`](file:///D:/Atlas/.agents/skills/karpathy-guidelines/SKILL.md): Think before coding, simplicity first, surgical scope, and verifiable success criteria.
5. [`ponytail`](file:///D:/Atlas/.agents/skills/ponytail/SKILL.md): Senior developer laziness — YAGNI, standard library and ESP-IDF native features first, refusing speculative complexity.
6. [`ponytail-audit`](file:///D:/Atlas/.agents/skills/ponytail-audit/SKILL.md): Pre-planning audit to prevent bloat, redundant implementations, and dead abstractions.
7. [`slopmonster`](file:///D:/Atlas/.agents/skills/slopmonster/SKILL.md): Concrete planning prose free from AI fluff, marketing jargon, and hand-waving.
8. [`context7`](file:///D:/Atlas/.agents/skills/context7/SKILL.md): Authoritative documentation and API lookup via `ctx7 library` and `ctx7 docs`. Research architectural feasibility, verify ESP-IDF and FreeRTOS documentation, check current API capabilities, and compare library versions. Strictly research-only during planning.

**Explicit Exclusions:**
- **`diagnosing-bugs`**: DO NOT ACTIVATE during planning. Bug diagnosis belongs exclusively to `/debug`.
- **`setup-matt-pocock-skills`**: DO NOT ACTIVATE. Repository setup is dormant.

### Enhanced Planning Procedure
1. **Read Project Instructions & Rules:** Consult `docs/ARCHITECTURE.md`, `docs/OTA_AND_LAUNCH_AUDIT.md`, `docs/STORAGE_DESIGN.md`, active ECC rules in [`.agents/rules/`](file:///D:/Atlas/.agents/rules/README.md), and relevant subsystem headers.
2. **Load the 8 Analytical Skills:** Read each assigned skill's `SKILL.md` and apply its principles.
3. **Inspect the Actual Codebase:** Read relevant source files and headers before proposing any architectural change.
4. **Understand Existing Architecture:** Trace subsystem boundaries, FreeRTOS tasks, event flows, and driver interactions.
5. **Map Implementation State:** Clearly distinguish implemented, partially implemented, and proposed functionality.
6. **Detect Duplicate & Unnecessary Abstractions:** Audit existing modules using Ponytail and Codebase Design to avoid reinventing existing patterns.
7. **Analyze Architectural Friction:** Ensure proposed changes increase module depth and preserve locality.
8. **Audit for Slop:** Keep proposed interfaces minimal, clean, and free from speculative configuration layers.
9. **Evaluate Hardware Constraints:** Validate against embedded memory, DMA buffers, flash wear, and CPU scheduling.
10. **Identify Risks & Trade-Offs:** Explicitly list affected subsystems, breaking changes, and prerequisites.
11. **Develop an Actionable Implementation Plan:** Specify exact files to create, modify, or delete with concrete interface signatures.
12. **Specify Verification Requirements:** Define automated build commands (`pio run`), regression checks, and physical hardware validation steps.
13. **Distinguish Facts from Assumptions:** Explicitly label verified facts, working assumptions, and proposed modifications.
14. **Present the Plan:** Emit the `implementation_plan.md` artifact and halt for user feedback.

---

## 2. ATLAS Embedded Architecture Context & Constraints

Every plan, architectural proposal, and code modification must strictly respect ATLAS hardware realities:

- **Operating System Nature:** ATLAS is an embedded operating system inspired by Linux architecture principles (VFS, event loops, services, distinct application isolation), **not** a Linux distribution. It runs directly on bare metal using FreeRTOS.
- **Target Silicon:** ESP32-S3 (Xtensa Dual-Core 240MHz, 512KB SRAM, 8MB Octal PSRAM, 16MB SPI Flash).
- **Core Subsystems:**
  - `boot/`: Multi-stage bootloader sequence (`atlas_boot.c/.h`).
  - `core/`: Event loop (`atlas_event`), logging (`atlas_log`), service registry (`atlas_service`), hardware telemetry (`atlas_sysinfo`).
  - `drivers/`: ILI9341 LCD via SPI DMA, joystick ADC1 + GPIO buttons, I2S audio (DAC TX / Mic RX), SDSPI FatFS.
  - `system/`: Application registry/manager (`app_manager`), OTA engine (`atlas_ota`).
  - `ui/`: Framebuffer rendering (`atlas_gfx`), home shell (`screen_home`), recovery mode (`screen_recovery`).
  - `APPS/`: Git submodule application catalogue.

### Mandatory Architectural Constraints
1. **Subsystem Responsibility Separation:** Keep application runtime, package management, application management, and firmware OTA responsibilities strictly distinct:
   - Firmware image flashing is managed exclusively by `atlas_ota`.
   - Application catalogue and metadata are managed by `app_manager`.
   - Never conflate updating OS firmware with installing an application package.
2. **OTA Safety & Partition Management:** Dual-slot safety (`ota_0`/`ota_1`) must be enforced by `atlas_ota`, including pre-flash image validation and rollback confirmation. Never stage raw binaries directly into active or unverified OTA slots.
3. **Application Execution Truthfulness:** Never treat arbitrary `.bin` files as safely executable applications. Third-party applications must not be described as executable until their runtime environment (e.g., WASM or interpreted sandbox) is fully implemented and verified on the hardware.
4. **Manifest Permissions Reality:** Manifest permissions are documentation metadata unless runtime hardware enforcement mechanisms exist.
5. **Catalogue Repository Role:** `APPS/` is a community application catalogue and source repository, never a firmware binary dumping ground.
6. **Storage Safety:** Never format the SD card or destroy user data. Mount safely at `/sdcard`.
7. **Hardware Pin Integrity:** Never assume hardware pin assignments. Always verify pins against `main/include/atlas_pins.h` and schematics.
8. **Compilation vs Physical Validation:** Distinguish clean compilation from physical hardware validation. Successful `pio run` is a prerequisite, not proof of peripheral timing, bus contention safety, or DMA stability.
9. **Memory Allocation & Scheduling:** Internal SRAM is scarce (512KB); use 8MB PSRAM for large framebuffers and allocations via `MALLOC_CAP_SPIRAM`. Reserve internal SRAM for DMA buffers (`MALLOC_CAP_DMA`). Respect FreeRTOS task stack budgets and priority inversion rules.
10. **Zero Unverified Claims:** Do not claim a runtime, feature, or hardware behavior exists without verifying its implementation in the source tree. Do not introduce desktop-OS complexity (e.g. dynamic MMU processes, monolithic abstractions) without demonstrated embedded need.

---

## 3. Orchestrator Registry & Skill Inventory

The ATLAS project provides exactly three orchestrated engineering workflows:

1. **Native `/plan`** (Plan Mode): Analytical architectural evaluation and implementation planning. Regulated by Section 1 above. Activates the 8 analytical skills (`codebase-design`, `domain-modeling`, `improve-codebase-architecture`, `karpathy-guidelines`, `ponytail`, `ponytail-audit`, `slopmonster`, `context7`). Strictly planning-only.
2. **`/code`** (`.agents/skills/code/SKILL.md`): General implementation and feature development. Activates the 8 coding and analysis skills (`codebase-design`, `domain-modeling`, `improve-codebase-architecture`, `karpathy-guidelines`, `ponytail`, `ponytail-audit`, `slopmonster`, `context7`). Governed by project instructions and applicable ECC rules.
3. **`/debug`** (`.agents/skills/debug/SKILL.md`): Defect investigation, crash diagnosis, and bug fixing. Activates 9 skills (`diagnosing-bugs` + the 8 coding/analysis skills). Reproduces defects before code modification, isolates root causes, and verifies fixes with builds and tests.

### Preserved Skill Inventory
The project maintains exactly nine installed specialist skills plus Context7:
1. `codebase-design`
2. `diagnosing-bugs`
3. `domain-modeling`
4. `improve-codebase-architecture`
5. `karpathy-guidelines`
6. `ponytail`
7. `ponytail-audit`
8. `setup-matt-pocock-skills` (Explicitly **dormant**; never activated through any orchestrator)
9. `slopmonster`
10. `context7` (Documentation lookup skill via `ctx7` CLI)

No additional orchestrators or master skills exist.

---

## 4. Engineering Rules Inventory (ECC Ruleset)

The project rules in [`.agents/rules/`](file:///D:/Atlas/.agents/rules/README.md) are verbatim copies imported from Affaan Mustafa's authoritative **Everything Claude Code (ECC)** repository (commit `b38be6a7f6350964d59328b52f0624e1d1d552d1`, MIT License).

### Active Rule Files

#### Universal Rules (`.agents/rules/common/`)
- [`common/coding-style.md`](file:///D:/Atlas/.agents/rules/common/coding-style.md): Core coding principles — KISS, DRY, YAGNI, file length bounds, error handling, input validation, and code smells.
- [`common/code-review.md`](file:///D:/Atlas/.agents/rules/common/code-review.md): Code review standards, mandatory review triggers, checklist, security review triggers, and severity levels.
- [`common/development-workflow.md`](file:///D:/Atlas/.agents/rules/common/development-workflow.md): Feature pipeline — Research & Reuse (Context7), Plan first, TDD, Review, and Commit discipline.
- [`common/hooks.md`](file:///D:/Atlas/.agents/rules/common/hooks.md): Tool execution hooks, permissions, safety checks, and task tracking discipline.
- [`common/patterns.md`](file:///D:/Atlas/.agents/rules/common/patterns.md): Skeleton project structures, repository patterns, and structured interface formats.
- [`common/performance.md`](file:///D:/Atlas/.agents/rules/common/performance.md): AI assistant context window management, extended thinking, plan mode efficiency, and build troubleshooting.
- [`common/security.md`](file:///D:/Atlas/.agents/rules/common/security.md): Mandatory security audit triggers, credential protection, input validation, and security response protocols.
- [`common/testing.md`](file:///D:/Atlas/.agents/rules/common/testing.md): Test-driven development (TDD), red-green-refactor loop, test coverage targets, and AAA test organization.
- [`common/agents.md`](file:///D:/Atlas/.agents/rules/common/agents.md): Subagent orchestration, delegation contracts, and parallel execution standards.

#### C/C++ Embedded Rules (`.agents/rules/cpp/`)
- [`cpp/coding-style.md`](file:///D:/Atlas/.agents/rules/cpp/coding-style.md): Modern C++ (C++17/20), RAII resource management, naming conventions, and clang-format enforcement.
- [`cpp/patterns.md`](file:///D:/Atlas/.agents/rules/cpp/patterns.md): C++ design patterns — RAII lifetime management, Rule of Five/Zero, value semantics, and structured error handling.
- [`cpp/security.md`](file:///D:/Atlas/.agents/rules/cpp/security.md): C++ memory safety, buffer overflow prevention, avoiding undefined behavior, sanitizers, clang-tidy, and cppcheck.
- [`cpp/testing.md`](file:///D:/Atlas/.agents/rules/cpp/testing.md): C++ testing with GoogleTest, CTest, code coverage, and sanitizer integration.
- [`cpp/hooks.md`](file:///D:/Atlas/.agents/rules/cpp/hooks.md): C++ static analysis and build verification hooks (clang-format, clang-tidy, cppcheck, build & test checks).

---

## 5. Conditional Application of ECC Rules to ATLAS

ECC rules provide valuable universal and language-level guidance, but must be applied **conditionally** to match ATLAS embedded architecture realities:

1. **Build Tooling:** ECC C++ rules mention `cmake` and `ctest`. The ATLAS build system is **PlatformIO (`pio run`)**. Do not inject desktop CMake configurations or invoke `ctest` directly; always build and verify through PlatformIO.
2. **Testing Harness:** ECC rules mention GoogleTest. ATLAS embedded tests run on target hardware or in QEMU via PlatformIO test runners (`pio test`) and ESP-IDF Unity testing harnesses.
3. **Exceptions & RTTI:** ECC rules mention exceptions (`throw`, `std::expected`). ESP-IDF disables C++ exceptions and RTTI by default (`-fno-exceptions`, `-fno-rtti`) to minimize flash footprint and prevent unhandled aborts. Error handling in ATLAS must use ESP-IDF status codes (`esp_err_t`), `std::optional`, or structured result types.
4. **Memory Allocation:** ECC rules state "never use malloc/free". In ATLAS, C driver code and DMA operations strictly require ESP-IDF capability allocators (`heap_caps_malloc(..., MALLOC_CAP_DMA)` for SPI DMA buffers, `MALLOC_CAP_SPIRAM` for PSRAM framebuffers). When authoring C++, wrap these allocations in RAII containers or `std::unique_ptr` with custom deleters rather than using generic desktop `new`.

---

## 6. Orchestrator Rule Governance Matrix

| Orchestrator | Primary Rules Consulted | Purpose |
|---|---|---|
| **Native `/plan`** | [`common/development-workflow.md`](file:///D:/Atlas/.agents/rules/common/development-workflow.md)<br>[`common/patterns.md`](file:///D:/Atlas/.agents/rules/common/patterns.md)<br>[`common/performance.md`](file:///D:/Atlas/.agents/rules/common/performance.md)<br>[`cpp/patterns.md`](file:///D:/Atlas/.agents/rules/cpp/patterns.md)<br>[`common/security.md`](file:///D:/Atlas/.agents/rules/common/security.md) | Ensure plans enforce structured development pipelines, RAII resource patterns, context efficiency, and security considerations. |
| **`/code`** | [`common/coding-style.md`](file:///D:/Atlas/.agents/rules/common/coding-style.md)<br>[`cpp/coding-style.md`](file:///D:/Atlas/.agents/rules/cpp/coding-style.md)<br>[`cpp/patterns.md`](file:///D:/Atlas/.agents/rules/cpp/patterns.md)<br>[`common/security.md`](file:///D:/Atlas/.agents/rules/common/security.md)<br>[`cpp/security.md`](file:///D:/Atlas/.agents/rules/cpp/security.md)<br>[`common/testing.md`](file:///D:/Atlas/.agents/rules/common/testing.md)<br>[`cpp/testing.md`](file:///D:/Atlas/.agents/rules/cpp/testing.md) | Enforce modern C/C++ style, RAII, memory safety, buffer bounds checking, and testing discipline under PlatformIO constraints. |
| **`/debug`** | [`common/code-review.md`](file:///D:/Atlas/.agents/rules/common/code-review.md)<br>[`common/testing.md`](file:///D:/Atlas/.agents/rules/common/testing.md)<br>[`cpp/testing.md`](file:///D:/Atlas/.agents/rules/cpp/testing.md)<br>[`cpp/security.md`](file:///D:/Atlas/.agents/rules/cpp/security.md)<br>[`cpp/hooks.md`](file:///D:/Atlas/.agents/rules/cpp/hooks.md) | Review checklist and severity assessment, construct red reproduction tests before editing, verify memory safety, and run static analysis checks. |
