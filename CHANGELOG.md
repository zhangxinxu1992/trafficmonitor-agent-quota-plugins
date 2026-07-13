# Changelog

## v1.7

### 新增

- 新增 `TrafficMonitorClaudeQuota.dll`，提供 `CL 5h:`、`CL 7d:` 和 `CL 1mo:` 三个 Claude 额度显示项。
- 支持通过 Claude Web `sessionKey` 读取与设置页面一致的 5 小时、7 天和 Enterprise 月度 spend limit，并将凭据安全保存到 Windows 凭据管理器。
- 未配置网页会话时，支持回退到 Claude Code OAuth usage endpoint。
- 新增 Claude parser、Release x64 单元测试、插件冒烟测试和可选实时测试。

### 变更

- Release ZIP 和 GitHub Actions 发布流程加入 Claude 插件及测试项目。

**完整变更记录**：https://github.com/zhangxinxu1992/trafficmonitor-agent-quota-plugins/compare/v1.6...v1.7

## v1.6

### 新增

- 新增 `CX 1mo:` TrafficMonitor 显示项，用于展示 Codex Business 和 Enterprise 账户返回的工作区月度支出控制额度。
- 补充解析器、插件冒烟测试和实时测试覆盖，支持用量响应中不包含 5 小时或 7 天限额窗口的账户。

### 变更

- 成功刷新后，不可用的 Codex 额度项现在显示 `N/A`，不再持续显示加载占位符。
- Codex 额度请求现在支持 `HTTPS_PROXY` 和 `HTTP_PROXY`，并在未设置环境变量时回退到 Windows 自动代理配置。

**完整变更记录**：https://github.com/zhangxinxu1992/trafficmonitor-agent-quota-plugins/compare/v1.5...v1.6

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
