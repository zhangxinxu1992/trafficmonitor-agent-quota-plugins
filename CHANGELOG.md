# Changelog

## v1.8

### 新增

- 新增 `TrafficMonitorClaudeQuota.dll`，提供 `CL 5h:`、`CL 7d:` 和 `CL 1mo:` 三个 Claude 额度显示项。
- 支持读取 Claude Code OAuth usage endpoint 返回的 5 小时、7 天和 Enterprise `extra_usage` 月度 spend limit。
- Claude Options 在未登录时先引导并执行 `claude auth login`，成功后才进入显示设置。

### 变更

- Release ZIP 和 GitHub Actions 发布流程加入 Claude 插件及测试项目。

### 修复

- 修复 Claude Options 窗口在高 DPI 显示器上仍使用固定像素布局，导致文字、输入框和按钮被裁切的问题。
- 修复 `.credentials.json` 中 `mcpOAuth` 位于 `claudeAiOauth` 前面时，插件误读空 MCP `accessToken` 并显示 `ERR` 的问题。

### 验证

- 新增 Claude Options 窗口 DPI 缩放、控件边界和字体大小的插件冒烟回归测试。
- 新增 MCP OAuth token 排在 Claude OAuth token 前面的认证解析回归测试。
- 新增 Claude Release x64 单元测试、插件冒烟测试和可选实时测试。

**完整变更记录**：https://github.com/zhangxinxu1992/trafficmonitor-agent-quota-plugins/compare/v1.7...v1.8

## v1.7

### 修复

- 修复 ChatGPT 临时取消 5 小时额度限制时，唯一的 7 天窗口被错误显示为 `CX 5h:`、而 `CX 7d:` 显示 `N/A` 的问题。
- Codex 5 小时和 7 天窗口现在根据 `limit_window_seconds` 归类；周期字段缺失时仍兼容原有的 primary/secondary 位置映射。
- 月度工作区额度继续通过 `spend_control.individual_limit` 识别，不依赖不存在的固定月度周期秒数。

### 验证

- 新增仅有 7 天 primary 窗口、窗口位置互换和周期字段缺失的回归测试，加强实时测试对窗口周期的断言，并让实时插件冒烟等待范围覆盖 WinHTTP 网络超时。

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
