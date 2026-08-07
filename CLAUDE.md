# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

This is **not an application codebase** — it is a Claude Code plugin marketplace that packages *engineering skills* for ACAP application development. There is no build, test, or lint step; the deliverable is Markdown that Claude reads at runtime. Editing here means writing/curating skill documentation, not compiling code.

## Structure

skills  -> (SKILL.md per directory)

## Conventions for editing skills

- Every skill lives in skills/<name>/SKILL.md
- YAML frontmatter with name and description fields
- Description starts with what the skill does (third person), followed by trigger conditions ("Use when...")
- **SKILL.md is a router, not an encyclopedia.** It carries the workflow (skeleton first, then
  features), the scripts, the manifest basics, and the API table that points at `references/`.
  Anything API-specific — call sequences, struct lifetimes, gotchas — belongs in the reference
  file, not inline. If a section of SKILL.md is only relevant once you've picked an API, it is in
  the wrong file.
- **Each reference file follows a fixed shape:**

  ```
  # <Name> — <Purpose> API
  <2–5 lines: what it does and when you'd reach for it>
  **API specification:** <developer.axis.com URL>
  <optional: ⚠️ callout for a trap that forces an early either/or, e.g. bbox vs overlay>

  ## Build Requirements
  ### Makefile        <- the PKGS line (plus LDLIBS if linking is unusual)
  ### Source files    <- the #include set
  ### manifest.json   <- the declaration, or "Nothing required" when there is none
  ## Core objects     <- table of the handle types and what each owns
  ## <workflow sections>
  ## Notes & gotchas
  ## Related
  ```

  The three `Build Requirements` subsections exist because each snippet lands in a *different
  file*. A bare ```` ```make ````/```` ```c ````/```` ```json ```` run makes the reader infer the
  destination from the fence language, and the manifest is the one that gets misplaced.
- **`manifest.json` snippets are the delta, rooted at their top-level key.** Write the fragment
  starting from `"resources": {` (or `"acapPackageConf": {` for AXParameter and License Key) and
  stop there — no `schemaVersion`, no sibling stubs. The root key is what makes the destination
  unambiguous; everything above it is boilerplate that would repeat across every reference file.
  Correct placement is enforced structurally instead: the skeleton manifest in `SKILL.md` ships
  `"resources": {}` already open at the right nesting level, so adding an API means filling an
  existing object rather than deciding where to create one.

## Domain notes (building-acap skill)

- ACAP apps are C applications cross-compiled in Docker for `aarch64` (default) or `armv7hf`, producing an `.eap` package. `appName` must be identical across `manifest.json`, the `.c` source filename, and the binary.
- The most common runtime failure is a missing per-API declaration in `manifest.json` (e.g. `video` user group for VDO/Larod, D-Bus methods for Bounding Box). The API table in `SKILL.md` is the authoritative checklist.
