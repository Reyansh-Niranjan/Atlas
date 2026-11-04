---
name: debug
description: Systematic debugging, root-cause analysis, and defect correction workflow for the ATLAS project. Activates diagnosing-bugs, codebase-design, domain-modeling, improve-codebase-architecture, karpathy-guidelines, ponytail, ponytail-audit, slopmonster, and context7.
---

# `/debug` — ATLAS Defect Diagnosis & Resolution Workflow

Use this orchestrator for defect investigation, bug fixing, crash diagnosis, and panic resolution in the ATLAS firmware project (`D:\Atlas`).

This workflow coordinates nine specialist skills to isolate root causes with tight feedback loops and apply minimal, technically justified fixes.

---

## 1. Skill Activation Manifest

### Exactly Activated Skills (9)
You **MUST** explicitly load and apply the following nine specialist skills:
1. [`diagnosing-bugs`](file:///D:/Atlas/.agents/skills/diagnosing-bugs/SKILL.md): Rigorous diagnostic loops, red-capable reproduction signals, hypothesis testing, and isolation of root causes before editing.
2. [`codebase-design`](file:///D:/Atlas/.agents/skills/codebase-design/SKILL.md): Identification of seam boundaries, interface contracts, error modes, and state invariants.
3. [`domain-modeling`](file:///D:/Atlas/.agents/skills/domain-modeling/SKILL.md): Truthful terminology, verification of documented behavior vs reality, and constraint alignment.
4. [`improve-codebase-architecture`](file:///D:/Atlas/.agents/skills/improve-codebase-architecture/SKILL.md): Identifying structural friction, leaking state, or shallow abstractions causing faults.
5. [`karpathy-guidelines`](file:///D:/Atlas/.agents/skills/karpathy-guidelines/SKILL.md): Surgical fixes touching only what is required, simplicity first, avoiding speculative refactors during a bugfix.
6. [`ponytail`](file:///D:/Atlas/.agents/skills/ponytail/SKILL.md): Root cause over symptom; smallest change that resolves the defect; no new abstractions for a bug fix.
7. [`ponytail-audit`](file:///D:/Atlas/.agents/skills/ponytail-audit/SKILL.md): Verification that fixes do not introduce dead code, bloat, or complexity creep.
8. [`slopmonster`](file:///D:/Atlas/.agents/skills/slopmonster/SKILL.md): Clean documentation of the defect and fix without AI boilerplate.
9. [`context7`](file:///D:/Atlas/.agents/skills/context7/SKILL.md): Authoritative documentation and error lookup via `ctx7 library` and `ctx7 docs`:
   - Investigating API behavior.
   - Checking version-specific changes and known limitations.
   - Verifying expected framework behavior.
   - Researching documented error conditions and configuration requirements.
   - *Constraint:* Auxiliary only — never replaces the diagnostic loop. Only query Context7 after reproducing the defect and isolating the failure point to an external API interaction.

### Explicitly Excluded Skills
- **`setup-matt-pocock-skills`**: DO NOT ACTIVATE.

---

## 2. ATLAS Embedded Architecture Context & Constraints

Embedded debugging on the ESP32-S3 requires hardware-truthful analysis:

- **Target Silicon:** ESP32-S3 (Xtensa Dual-Core 240MHz, 512KB SRAM, 8MB Octal PSRAM, 16MB Flash).
- **Embedded Failure Modes to Check:**
  - Stack overflow in FreeRTOS tasks (check configured task stack depth in `xTaskCreateWithCaps` or `xTaskCreate`).
  - Heap corruption or allocating DMA buffers in PSRAM (DMA operations require internal SRAM with `MALLOC_CAP_DMA`).
  - Priority inversion, deadlocks, or recursive mutex acquisition in FreeRTOS synchronization primitives.
  - Watchdog timer timeouts (Task Watchdog Timer `TWDT` or Interrupt Watchdog `IWDT`) caused by non-yielding CPU-bound loops.
  - Bus errors or cache errors when accessing external SPI flash/PSRAM while flash write/erase operations are active.
  - Pin conflicts or SPI bus contention between ILI9341 LCD and MicroSD card on shared SPI2 bus (`atlas_pins.h`).

### Strict Debugging Prohibitions
**NEVER:**
- Randomly modify code until an error symptom disappears.
- Suppress or hide compiler warnings with `#pragma` or cast hacks.
- Disable, comment out, or weaken failing assertions or tests to achieve a passing build.
- Replace an entire subsystem without concrete evidence proving it is flawed.
- Claim a fix works without reproducing the problem and demonstrating verified resolution.

---

## 3. Step-by-Step `/debug` Execution Workflow

1. **Read ATLAS Project Instructions & Rules:** Consult `docs/ARCHITECTURE.md`, `docs/OTA_AND_LAUNCH_AUDIT.md`, `AGENTS.md`, and applicable ECC rules in [`.agents/rules/`](file:///D:/Atlas/.agents/rules/README.md) (notably code-review checklist and C++ security/testing hooks).
2. **Load and Apply Assigned Skills:** Read the 9 assigned skill files and internalize the diagnosis discipline.
3. **Collect Failure Information:** Capture exact compiler error messages, backtraces, registers (`MEPC`, `EXCVADDR`), ESP-IDF panic logs, or symptom descriptions. Redact credentials/tokens if present.
4. **Reproduce the Defect:**
   - Build a **tight feedback loop** (Phase 1 of `diagnosing-bugs`): an automated build command (`pio run`), a targeted test, or a deterministic reproduction script that reliably goes red.
   - If non-deterministic, tighten timing or increase test iterations.
5. **Inspect Diagnostics & Source:** Examine serial logs, task states, register dumps, and the specific code path leading to the fault.
6. **Trace Execution Path:** Follow the call stack across subsystem seams and FreeRTOS task boundaries.
7. **Identify Root Cause:** Determine why the invariant was violated (e.g. uninitialized pointer, memory leak, race condition, pin collision, buffer overflow).
8. **Context7 Usage Policy & API Lookup (Auxiliary Only):** If the root cause points to API contract misuse, undocumented framework quirks, or version discrepancies:
   1. *Identify Dependency & Version:* Check the exact ESP-IDF v6.0.1 component or FreeRTOS API involved in the failure.
   2. *Resolve Context7 Library:* Run `npx ctx7@latest library <name> "<query>"` to locate the authoritative docs.
   3. *Query Error Specifications:* Run `npx ctx7@latest docs <libraryId> "<function/error>"` to review documented return codes (`esp_err_t`), precondition asserts, and parameter validation.
   4. *Prefer Exact Project Version:* Compare behavior against ESP-IDF v6.0 release documentation.
   5. *Cross-Check Against Codebase:* Re-verify against actual hardware state, task priorities, and register backtraces.
   - *Strict Boundary:* Context7 is auxiliary only and must NEVER replace the systematic reproduction and diagnostic loop.
9. **Distinguish Facts from Hypotheses:** Explicitly state what is proven by evidence versus what remains an unverified hypothesis.
10. **Determine Smallest Justified Fix:**
    - Apply the **Ponytail rule**: Fix the root cause with the smallest surgical change.
    - Do NOT rewrite adjacent code, refactor surrounding modules, or add speculative features.
11. **Implement the Fix:** Make surgical, targeted code adjustments.
12. **Add or Update Regression Verification:** Ensure the failure condition is guarded by an assertion, validation check, or test.
13. **Run Builds & Validation:** Execute `pio run` to verify clean compilation with zero warnings.
14. **Audit Changes (Ponytail-Audit & SlopMonster):** Check the final diff for cleanliness, minimal line count, and no accidental regressions.
15. **Report Engineering Findings:**
    - Root cause analysis (proven mechanism of failure).
    - Affected files and line ranges.
    - Exact fix applied and why it is technically justified.
    - Verification evidence (how the failure loop now passes).
    - Remaining uncertainties or hardware-specific follow-ups.

