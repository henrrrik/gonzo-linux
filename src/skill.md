---
name: gonzo-writer
description: >
  Inspect, validate, and open Markdown documents with Gonzo, Omarchy's
  distraction-free Markdown writing app. Use when asked to check Markdown
  files for Gonzo compatibility, count words/headings/links, run spelling
  checks that skip inline code and link URLs, or open a file in Gonzo.
---

# Gonzo CLI

Gonzo (`gonzo-writer`) runs headless commands without a display and launches
the editor GUI when given a file or `open`.

## Commands

- `gonzo-writer [file]` or `gonzo-writer open [file]` — Open the editor window (optionally on `file`).
- `gonzo-writer stats [--json] <file>` — Report word count, characters, UTF-8 bytes, paragraphs, headings, and links.
- `gonzo-writer check [--json] [--spell] <file>` — Validate that `file` is a regular local UTF-8 file within Gonzo's 16 MB limit; with `--spell`, also report Hunspell misspellings outside inline code spans and link destinations.
- `gonzo-writer skill` — Print this skill.
- `gonzo-writer skill install` — Install this skill to `~/.agents/skills/gonzo-writer/SKILL.md`.

## Exit codes

- `0` on success (or clean check).
- `1` on invalid arguments, missing/unreadable/non-regular/non-UTF-8/oversized files, or `--spell` issues; errors are written to stderr (and included in JSON when `--json` is set).
