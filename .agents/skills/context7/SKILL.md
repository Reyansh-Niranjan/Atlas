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
- **Purpose:**
  - Researching architectural decisions.
  - Checking current API capabilities.
  - Verifying ESP-IDF and FreeRTOS documentation.
  - Comparing library versions and compatibility.
  - Researching unfamiliar technologies before recommending them.
- **Boundary:** Strictly research-only. Plans must ground designs in existing ATLAS patterns before proposing new APIs.

### In `/code`
- **Purpose:**
  - Looking up unfamiliar APIs before implementation.
  - Checking version-specific usage (ESP-IDF v6.0.1, FreeRTOS 1000Hz).
  - Verifying configuration options.
  - Avoiding outdated or hallucinated API usage.
  - Checking migration and compatibility requirements.
- **Boundary:** Prevents hallucinated or deprecated APIs. Never introduces unrequested external dependencies.

### In `/debug`
- **Purpose:**
  - Investigating API behavior.
  - Checking version-specific changes and known limitations.
  - Verifying expected framework behavior.
  - Researching documented error conditions and configuration requirements.
- **Boundary:** Auxiliary lookup only — must NEVER replace the systematic reproduction and diagnostic loop.

---

## 4. Mandatory Context7 Usage Policy

All three orchestrators must follow this mandatory five-step policy:

1. **Identify the actual dependency and version** used by ATLAS (ESP-IDF v6.0.1, FreeRTOS 1000Hz tick rate, PlatformIO).
2. **Resolve the appropriate Context7 library documentation** via `npx ctx7@latest library <name> "<query>"`.
3. **Query the relevant API or technical behavior** via `npx ctx7@latest docs <libraryId> "<query>"`.
4. **Prefer documentation matching the project's actual version** (e.g. `/websites/espressif_projects_esp-idf_en_release-v6_0`).
5. **Cross-check documentation against ATLAS source code and configuration** (`main/include/atlas_pins.h`, schematics).

> [!CAUTION]
> Do not blindly trust documentation over verified repository behavior. Context7 is a documentation research tool, not permission to introduce new dependencies or change architecture.

