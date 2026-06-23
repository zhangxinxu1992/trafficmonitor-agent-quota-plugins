# Release Changelog Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add release notes that are visible on future GitHub releases and backfill the already-published releases.

**Architecture:** Keep human-written release notes in `CHANGELOG.md`. Use a small PowerShell helper to extract the matching tag section during the GitHub Actions release flow, so a release cannot silently publish without notes.

**Tech Stack:** GitHub Actions, GitHub CLI, PowerShell, Markdown.

---

### Task 1: Changelog Extractor

**Files:**
- Create: `.github/scripts/Get-ReleaseNotes.ps1`
- Create: `tests/ReleaseNotesScriptTests.ps1`

- [ ] Add a script test that extracts one version section and verifies missing versions fail.
- [ ] Run the test and confirm it fails because `.github/scripts/Get-ReleaseNotes.ps1` is missing.
- [ ] Implement `.github/scripts/Get-ReleaseNotes.ps1` with exact `## vX.Y` section extraction.
- [ ] Run `.\tests\ReleaseNotesScriptTests.ps1` and confirm it passes.

### Task 2: Release Notes Source

**Files:**
- Create: `CHANGELOG.md`

- [ ] Add `CHANGELOG.md` with `v1.0` through `v1.4` sections.
- [ ] Keep entries factual and based on tag diffs.
- [ ] Include full changelog links for each release.

### Task 3: Workflow Integration

**Files:**
- Modify: `.github/workflows/release.yml`

- [ ] Add a workflow step that writes the extracted changelog section to `$RUNNER_TEMP\release-notes.md`.
- [ ] Change `gh release create` to use `--notes-file`.
- [ ] Change the existing-release path to also refresh notes with `gh release edit`.

### Task 4: Backfill Published Releases

**Files:**
- GitHub release records for `v1.0` through `v1.4`

- [ ] Generate one notes file per existing tag.
- [ ] Run `gh release edit <tag> --notes-file <file>` for each published release.
- [ ] Re-read release bodies and confirm the new notes are visible.
