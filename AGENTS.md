# AGENTS.md

## Project Context

This repository builds a TrafficMonitor x64 plugin that displays remaining Codex quota in the taskbar.

Primary references:

- TrafficMonitor plugin API: https://github.com/zhongyang219/TrafficMonitor/wiki/%E6%8F%92%E4%BB%B6%E5%BC%80%E5%8F%91%E6%8C%87%E5%8D%97
- Codex quota query behavior: https://github.com/Finesssee/Win-CodexBar

Read `README.md`, `docs/design.md`, and `docs/implementation-notes.md` before changing behavior.

## Display Rules

- Default to showing remaining quota; the plugin options can switch the display to used quota.
- Expose three display items:
  - `CX 5h:` for the Codex 5-hour primary window.
  - `CX 7d:` for the Codex 7-day secondary window.
  - `CX 1mo:` for the monthly workspace spend-control window used by company accounts.
- Default to reset countdowns in the value, for example `CX 5h: 69% 42m` and `CX 7d: 89% 6d 1h`; the plugin options can switch this to local reset time.
- Put the visible space after the colon at the start of the value text, not at the edge of the label. TrafficMonitor trims ordinary whitespace at plugin-label edges.

## Known Pitfalls

- `C:\Apps\TrafficMonitor\config.ini` is GBK encoded. Do not rewrite it as UTF-8, or Chinese labels may become garbled.
- TrafficMonitor caches plugin labels in `config.ini`; update `CodexQuota5h`, `CodexQuotaWeek`, and `CodexQuotaMonth` when labels change.
- Rebuild `TrafficMonitorCodexQuota.vcxproj` before running `PluginSmokeTests`; the smoke test loads `build\x64\Release\TrafficMonitorCodexQuota.dll`.
- TrafficMonitor locks the installed DLL while running. Stop it before copying a new DLL into `C:\Apps\TrafficMonitor\plugins`.
- Restarting TrafficMonitor from automation may require UAC confirmation.

## Verification

Use Release x64 builds.

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
& $msbuild .\TrafficMonitorCodexQuota.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\CodexQuotaTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
& $msbuild .\PluginSmokeTests.vcxproj /p:Configuration=Release /p:Platform=x64 /m
.\build\x64\Release\CodexQuotaTests.exe
.\build\x64\Release\PluginSmokeTests.exe
$env:TRAFFICMONITOR_CODEX_QUOTA_RUN_LIVE_TEST = '1'
.\build\x64\Release\CodexQuotaTests.exe
.\build\x64\Release\PluginSmokeTests.exe
```
