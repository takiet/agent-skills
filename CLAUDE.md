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
- **SKILL.md is a router, not an encyclopedia.** 
- **Each reference file follows a fixed shape:** 

## Domain notes (building-acap skill)

- ACAP apps are C applications cross-compiled in Docker for `aarch64` (default) or `armv7hf`, producing an `.eap` package. `appName` must be identical across `manifest.json`, the `.c` source filename, and the binary.
- The most common runtime failure is a missing per-API declaration in `manifest.json` (e.g. `video` user group for VDO/Larod, D-Bus methods for Bounding Box). The API table in `SKILL.md` is the authoritative checklist.
