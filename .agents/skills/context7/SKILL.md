---
name: context7
description: Fetch current library, framework, SDK, API, and platform documentation with the ctx7 CLI. Use for ESP-IDF, FreeRTOS, and embedded API lookups.
license: MIT
---

# Context7 Documentation Lookup

Use the `ctx7` CLI to fetch authoritative, up-to-date documentation whenever work involves an external library, framework, SDK, API, or platform service (e.g. ESP-IDF, FreeRTOS, LittleFS, PlatformIO, lwIP). This includes API syntax, configuration options, version migration, and documented error conditions.

> [!IMPORTANT]
> **Research-Only Discipline:**
> - Context7 provides documentation lookup only. It never grants permission to introduce unrequested dependencies, change repository architecture, or bypass approved implementation plans.
> - Always cross-check retrieved documentation against the actual ATLAS codebase, ESP32-S3 hardware constraints (`main/include/atlas_pins.h`), and the target framework version (**ESP-IDF v6.0.1**, FreeRTOS 1000Hz tick rate).

---

## 1. Documentation Lookup Workflow

Always follow the two-step resolution and fetch sequence:

### Step 1: Resolve the Library ID
Before querying documentation, resolve the exact Context7 library ID:
```bash
npx ctx7@latest library <name> "<what to look up>"
```
- Use the official library or component name with proper casing and punctuation (e.g., `esp-idf`, `freertos`, `littlefs`).
- Select the best match (format: `/org/project` or `/websites/...`) based on:
  1. Exact name match.
  2. Relevance to the specific subsystem.
  3. Version compatibility (e.g., ESP-IDF v6.x / release-v6_0).
  4. Benchmark score and reputation.

### Step 2: Fetch Documentation
Retrieve targeted documentation using the resolved library ID:
```bash
npx ctx7@latest docs <libraryId> "<what to look up>"
```
- Query specific concepts (e.g., `"gpio_config"`, `"i2s_channel_init"`, `"xTaskCreateWithCaps"`).
- Run separate queries for distinct concepts rather than combining unrelated topics.
- Limit queries to at most 3 commands per investigation to preserve context and quota.
- Never pass sensitive information (tokens, keys, passwords) in queries.

---

## 2. ATLAS Subsystem Lookup Reference

Common Context7 library IDs for the ATLAS platform:
- **ESP-IDF v6:** `/websites/espressif_projects_esp-idf_en_release-v6_0` or `/espressif/esp-idf`
- **FreeRTOS:** `/websites/freertos` or `/aws/amazon-freertos`
- **FatFS / SDSPI:** `/espressif/esp-idf` (FatFS peripheral integration)
- **LittleFS:** `/joltwallet/esp_littlefs`

---

## 3. Orchestrator Usage Guidelines

### In Native `/plan`
- **Purpose:** Architectural feasibility research, checking current API capabilities, verifying hardware driver support, and checking library compatibility.
- **Boundary:** Strictly research-only. Plans must ground designs in existing ATLAS patterns before proposing new APIs.

### In `/code`
- **Purpose:** Verifying exact function signatures, argument types, initialization structures, and configuration flags before writing code.
- **Boundary:** Prevents hallucinated or deprecated APIs from previous ESP-IDF versions (e.g. legacy I2S driver vs modern `esp_driver_i2s`).

### In `/debug`
- **Purpose:** Auxiliary lookup when root-cause diagnosis indicates possible API contract violations, unexpected error return codes, or undocumented framework edge cases.
- **Boundary:** Must NOT replace the systematic diagnostic loop in `/debug`. Only query Context7 after reproducing the issue and isolating the failure point to an external API interaction.

