# ATLAS Engineering Rules: ECC Attribution & Provenance

This directory contains engineering rules imported directly from Affaan Mustafa's **Everything Claude Code (ECC)** repository.

---

## 1. Upstream Provenance

- **Repository:** [https://github.com/affaan-m/ECC/](https://github.com/affaan-m/ECC/)
- **Author:** Affaan Mustafa
- **License:** MIT License
- **Commit SHA:** `b38be6a7f6350964d59328b52f0624e1d1d552d1`
- **Source Paths:** `rules/common/` and `rules/cpp/`
- **Destination:** `D:\Atlas\.agents\rules/` (`common/` and `cpp/`)
- **Fidelity:** 100% byte-for-byte verbatim copies (all SHA256 checksums verified).

---

## 2. Selected Rule Sets & Rationale

ATLAS is an embedded operating system and firmware project for ESP32-S3 (Xtensa Dual-Core 240MHz, ESP-IDF v6.0.1, FreeRTOS, PlatformIO, C11/C++17/C++20). The selected rule sets provide foundational engineering discipline and language-specific standards for C and C++ development.

### A. Common Rules (`common/`)

| File | Upstream Path | Selection Rationale |
|---|---|---|
| `agents.md` | `rules/common/agents.md` | Subagent orchestration, task delegation completion contracts, and parallel execution standards. |
| `code-review.md` | `rules/common/code-review.md` | Code review standards, mandatory triggers, comprehensive review checklist, security review triggers, and severity levels. |
| `coding-style.md` | `rules/common/coding-style.md` | Language-agnostic code quality principles: KISS, DRY, YAGNI, file length bounds, error handling, input validation, and code smells. |
| `development-workflow.md` | `rules/common/development-workflow.md` | Structured feature pipeline: Research & Reuse (Context7), Plan first, TDD, Review, and Commit discipline. |
| `hooks.md` | `rules/common/hooks.md` | Tool execution hooks, permissions, safety checks, and task tracking discipline. |
| `patterns.md` | `rules/common/patterns.md` | Architectural skeleton structures, repository patterns, and structured interface formats. |
| `performance.md` | `rules/common/performance.md` | AI assistant context window management, extended thinking, plan mode efficiency, and build troubleshooting. |
| `security.md` | `rules/common/security.md` | Mandatory security audit triggers, credential/secret protection, input validation, and security response protocols. |
| `testing.md` | `rules/common/testing.md` | Test-driven development (TDD), red-green-refactor loop, test coverage expectations, and AAA test organization. |

### B. C++ Rules (`cpp/`)

| File | Upstream Path | Selection Rationale |
|---|---|---|
| `coding-style.md` | `rules/cpp/coding-style.md` | Modern C++ (C++17/20), RAII resource management, naming conventions, and clang-format enforcement. |
| `hooks.md` | `rules/cpp/hooks.md` | C++ static analysis and build verification hooks (clang-format, clang-tidy, cppcheck, build & test checks). |
| `patterns.md` | `rules/cpp/patterns.md` | C++ design patterns: RAII lifetime management, Rule of Five/Zero, value semantics, and structured error handling. |
| `security.md` | `rules/cpp/security.md` | C++ memory safety, buffer overflow prevention, avoiding undefined behavior, sanitizers, clang-tidy, and cppcheck. |
| `testing.md` | `rules/cpp/testing.md` | C++ unit testing with GoogleTest, CTest, code coverage, and sanitizer integration. |

---

## 3. Excluded Rule Sets & Rationale

The following 20 language- and framework-specific rule directories from ECC were omitted:

| Excluded Directory | Target Technologies | Reason for Exclusion |
|---|---|---|
| `angular/` | Angular, TypeScript, web frontend | Not used in ATLAS embedded firmware. |
| `arkts/` | HarmonyOS, ArkTS | Not used in ATLAS embedded firmware. |
| `csharp/` | C#, .NET runtime | Not used in ATLAS embedded firmware. |
| `dart/` | Dart, Flutter mobile/web | Not used in ATLAS embedded firmware. |
| `fsharp/` | F#, .NET functional | Not used in ATLAS embedded firmware. |
| `golang/` | Go systems/backend | Not used in ATLAS embedded firmware. |
| `java/` | Java, JVM, Android | Not used in ATLAS embedded firmware. |
| `kotlin/` | Kotlin, Android | Not used in ATLAS embedded firmware. |
| `nuxt/` | Nuxt 4, Vue full-stack | Not used in ATLAS embedded firmware. |
| `perl/` | Perl scripting | Not used in ATLAS embedded firmware. |
| `php/` | PHP, backend web | Not used in ATLAS embedded firmware. |
| `python/` | Python runtime | Not used in ATLAS embedded firmware. |
| `react/` | React, web UI | Not used in ATLAS embedded firmware. |
| `react-native/` | React Native, Expo mobile | Not used in ATLAS embedded firmware. |
| `ruby/` | Ruby, Rails web | Not used in ATLAS embedded firmware. |
| `rust/` | Rust language | Not used in current ATLAS C/C++ firmware codebase. |
| `swift/` | Swift, Apple platforms | Not used in ATLAS embedded firmware. |
| `typescript/` | TypeScript, Node.js | Not used in ATLAS embedded firmware. |
| `vue/` | Vue 3 web frontend | Not used in ATLAS embedded firmware. |
| `web/` | HTML/CSS web frontend | Not used in ATLAS embedded firmware. |

---

## 4. Structural Integrity & Relative Paths

Per ECC specification, the rule directories are organized hierarchically (`common/` and `cpp/`) rather than flattened. This ensures:
1. No name collisions between common and language-specific rules sharing identical filenames (`coding-style.md`, `patterns.md`, `security.md`, `testing.md`, `hooks.md`).
2. Relative markdown references inside language rules (e.g. `[common/coding-style.md](../common/coding-style.md)`) resolve directly and correctly to `.agents/rules/common/`.

---

## 5. Upstream MIT License Notice

```text
MIT License

Copyright (c) 2026 Affaan Mustafa

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
