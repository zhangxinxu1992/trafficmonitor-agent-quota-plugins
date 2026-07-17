# TrafficMonitor Codex and Claude Quota Plugin Design

## Goal

Build a x64 TrafficMonitor plugin DLL that shows the user's Codex quota percentage in the taskbar and main TrafficMonitor display.

The plugin exposes three display items:

- `CX 5h:`: percentage of the Codex 5-hour primary rate window plus reset information.
- `CX 7d:`: percentage of the Codex 7-day secondary rate window plus reset information.
- `CX 1mo:`: percentage of the monthly workspace spend-control window returned for company accounts plus reset information.

The separate Claude plugin mirrors the same three-window shape with `CL 5h:`,
`CL 7d:`, and `CL 1mo:`. Enterprise accounts that expose only a monthly spend
limit show `N/A` for the unavailable 5-hour and 7-day items.

Values use compact text, for example `CX 5h: 76% 42m` or `CX 7d: 90% 6d 1h`. By default the percentage is remaining quota and the suffix is the countdown until that quota window resets. The user can switch the percentage to used quota, hide reset information, or show visible reset information as local reset time. The taskbar value text starts with a regular space so spacing remains visible after TrafficMonitor trims plugin-label edges. `GetItemValueSampleText()` follows the current display mode: countdown mode uses compact samples such as ` 100% 4h 59m` or ` 100% 6d 23h`, reset-time mode reserves enough width for values such as ` 100% 12-31 23:59`, and hidden reset mode reserves only ` 100%`.

When TrafficMonitor's taskbar resource usage graph is enabled, all Codex
items opt into the host graph API. The graph value always represents used
quota: it returns `used_percent` regardless of whether the text is configured
to show remaining or used quota. Before the first successful refresh, or when a
window is not available, the graph value is `0.0`.

## Data Source

The plugin follows the Win-CodexBar Codex provider approach:

1. Read Codex credentials from `%CODEX_HOME%\auth.json` when `CODEX_HOME` is set, otherwise `%USERPROFILE%\.codex\auth.json`.
2. Use the `OPENAI_API_KEY` field from the Codex auth file if present, otherwise use `tokens.access_token`.
3. Add `ChatGPT-Account-Id` when `tokens.account_id` is present.
4. Use `HTTPS_PROXY` or `HTTP_PROXY` when either environment variable contains an HTTP proxy; otherwise use the Windows system proxy configuration.
5. Send a read-only HTTPS GET request to `https://chatgpt.com/backend-api/wham/usage`.
6. Parse:
   - `rate_limit.primary_window.used_percent`
   - `rate_limit.primary_window.limit_window_seconds`
   - `rate_limit.primary_window.reset_at`
   - `rate_limit.secondary_window.used_percent`
   - `rate_limit.secondary_window.limit_window_seconds`
   - `rate_limit.secondary_window.reset_at`
   - `spend_control.individual_limit.used_percent`
   - `spend_control.individual_limit.remaining_percent`
   - `spend_control.individual_limit.limit`
   - `spend_control.individual_limit.used`
   - `spend_control.individual_limit.reset_at`

Personal accounts normally return the 5-hour primary and 7-day secondary
windows. A verified Business response instead returned `rate_limit: null` and
`spend_control.individual_limit` with source `workspace_spend_controls`. That
object represents a calendar-month workspace allocation. The parser prefers
`used_percent`, falls back to `100 - remaining_percent`, and finally derives
the percentage from `used / limit`.

个人账户的限额窗口按 `limit_window_seconds` 归类，而不是只依赖
`primary_window` / `secondary_window` 的字段位置：`18000` 秒对应 5 小时，
`604800` 秒对应 7 天。这样在 ChatGPT 临时取消 5 小时限制、仅把 7 天窗口放入
`primary_window` 时，插件仍会把它显示在 `CX 7d:`。响应缺少周期字段时才回退到
原有的位置映射；未知的正数周期不会冒充 5 小时或 7 天窗口。月度工作区额度仍由
`spend_control.individual_limit` 的结构识别，因为真实 Business 响应不提供
`limit_window_seconds`。

The first version intentionally ignores `additional_rate_limits`, including Spark-specific quota windows.

## Claude Data Source

The Claude plugin uses Claude Code OAuth as its only authentication and data
source:

1. Read `%USERPROFILE%\.claude\.credentials.json`.
2. Parse OAuth tokens, expiry, scopes, client ID, and `rateLimitTier` only from
   the `claudeAiOauth` object; unrelated `mcpOAuth` tokens must never be selected.
3. When the access token expires within five minutes, refresh it through
   `POST https://platform.claude.com/v1/oauth/token` using the same request shape
   as Claude Code. Persist rotated tokens and expiry atomically without changing
   unrelated credential objects. A usage response of HTTP 401 triggers at most
   one refresh and one retry.
4. Send `GET https://api.anthropic.com/api/oauth/usage` with the
   `anthropic-beta: oauth-2025-04-20` header.
5. Parse `five_hour` and `seven_day` utilization/reset windows.
6. Parse the embedded `extra_usage` monthly limit and used credits. When the
   response has no explicit reset timestamp, use the next UTC calendar-month
   boundary; this displays as 08:00 in GMT+8.

Opening the plugin options requires a valid Claude Code login. If credentials
are missing, the plugin offers to run `claude auth login` in a new console and
opens display settings only after the command succeeds and credentials can be
read.

A claude.ai web `sessionKey` provider is reserved as a possible future
supplement if OAuth no longer exposes required quota fields. It is intentionally
not implemented in the current version.

## Configuration

Optional Codex display configuration lives at `%APPDATA%\TrafficMonitorCodexQuota\config.json`:

```json
{
  "quota_display": "remaining",
  "reset_display": "countdown",
  "show_reset_info": true
}
```

`quota_display` accepts `remaining` or `used`. `show_reset_info` accepts `true` or `false`. `reset_display` accepts `countdown` or `time` and only affects the taskbar value when `show_reset_info` is `true`. Missing config uses the defaults above.

Claude uses the same display-only schema at
`%APPDATA%\TrafficMonitorClaudeQuota\config.json`. Authentication remains only
in Claude Code's own credentials file.

## Runtime Behavior

`ITMPlugin::DataRequired()` must not block TrafficMonitor on network I/O. It starts a background refresh when the cached snapshot is stale and no refresh is already running.

The refresh interval is five minutes after a successful fetch and one minute after an error.
When an HTTP 429 response includes `Retry-After`, the next attempt waits at least
that long rather than using the one-minute error interval.

The display item value is:

- `...` before the first fetch completes.
- `<n>% <reset>` when the matching window is available.
- `N/A` after a successful fetch when that account does not provide the matching window.
- `ERR` when the most recent refresh failed and no valid value is available.

The tooltip includes the plan, all three windows, reset information, last refresh status, and the last error message when present.

## Error Handling

- Missing `auth.json`: show an authentication error in the tooltip.
- Missing token: show an authentication error in the tooltip.
- HTTP 401 or 403: show that `codex login` is required.
- Other HTTP/network/parser failures: keep the last successful value if available and show the error in the tooltip.

## Implementation Shape

The Codex implementation uses a small shared C++17 core plus two binaries:

- `TrafficMonitorCodexQuota.dll`: the TrafficMonitor plugin.
- `CodexQuotaTests.exe`: console tests for credential parsing, usage JSON parsing, percent formatting, and reset countdown formatting.

The plugin uses WinHTTP for HTTPS and has no third-party runtime dependency.

Claude follows the same shape with `TrafficMonitorClaudeQuota.dll`,
`ClaudeQuotaTests.exe`, and `ClaudePluginSmokeTests.exe`. It reuses the common
display formatting and config schema while keeping Claude response parsing,
credential storage, and fetch behavior in dedicated source files.
