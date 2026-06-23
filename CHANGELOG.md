# Changelog

## v1.5

### Fixed

- Increased the GitHub Copilot countdown sample width so day-plus-hour reset text, such as `7d 19h`, is no longer clipped in the TrafficMonitor taskbar window.
- Added smoke-test coverage for Codex and GitHub Copilot sample-width behavior across countdown, reset-time, and hidden-reset display modes.

**Full Changelog**: https://github.com/zhangxinxu1992/trafficmonitor-agent-quota-plugins/compare/v1.4...v1.5

## v1.4

### Fixed

- TrafficMonitor resource usage graph values now always represent used quota for both Codex and GitHub Copilot, so low remaining quota appears as a mostly filled graph.
- Updated the English and Chinese documentation to describe the used-quota graph behavior.

**Full Changelog**: https://github.com/zhangxinxu1992/trafficmonitor-agent-quota-plugins/compare/v1.3...v1.4

## v1.3

### Added

- Added TrafficMonitor resource usage graph support for the Codex 5-hour item, Codex 7-day item, and GitHub Copilot item.
- Graph values are clamped to TrafficMonitor's `0.0` to `1.0` range and stay empty before the first successful quota snapshot.

**Full Changelog**: https://github.com/zhangxinxu1992/trafficmonitor-agent-quota-plugins/compare/v1.2...v1.3

## v1.2

### Added

- Added options for both plugins to hide reset countdown/reset time text while keeping quota percentage visible.
- Added `show_reset_info` config persistence for Codex and GitHub Copilot display settings.
- Added Simplified Chinese README content and included it in the release package.

### Changed

- Refreshed the public README and development docs for install, configuration, release package, and testing details.
- Updated TrafficMonitor sample-width behavior so hidden reset and hidden credit fields no longer reserve extra taskbar width.

**Full Changelog**: https://github.com/zhangxinxu1992/trafficmonitor-agent-quota-plugins/compare/v1.1...v1.2

## v1.1

### Changed

- Bumped the shared plugin version metadata to `1.1` for the tagged release.

**Full Changelog**: https://github.com/zhangxinxu1992/trafficmonitor-agent-quota-plugins/compare/v1.0...v1.1

## v1.0

### Added

- Initial public release of the TrafficMonitor agent quota plugins package.
- Added `TrafficMonitorCodexQuota.dll` with `CX 5h:` and `CX 7d:` taskbar items for Codex quota windows.
- Added `TrafficMonitorGitHubCopilotQuota.dll` with the `GC:` taskbar item for GitHub Copilot quota.
- Added display options for remaining/used percentage and reset countdown/local reset time.
- Added GitHub Copilot device sign-in, Windows Credential Manager token storage, and Copilot quota fetching.
- Added release automation that builds Release x64 DLLs, runs non-live tests, and uploads the ZIP package.

**Full Changelog**: https://github.com/zhangxinxu1992/trafficmonitor-agent-quota-plugins/commits/v1.0
