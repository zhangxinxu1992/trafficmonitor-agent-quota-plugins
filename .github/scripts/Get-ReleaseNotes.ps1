param(
    [Parameter(Mandatory = $true)]
    [string]$Tag,

    [string]$ChangelogPath = 'CHANGELOG.md',

    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $ChangelogPath)) {
    throw "Changelog file not found: $ChangelogPath"
}

$content = Get-Content -LiteralPath $ChangelogPath -Raw
$escapedTag = [System.Text.RegularExpressions.Regex]::Escape($Tag)
$pattern = "(?ms)^##\s+$escapedTag\s*\r?\n(?<body>.*?)(?=^##\s+|\z)"
$match = [System.Text.RegularExpressions.Regex]::Match($content, $pattern)

if (-not $match.Success) {
    throw "No changelog section found for tag '$Tag' in $ChangelogPath."
}

$body = $match.Groups['body'].Value.Trim()
if ([string]::IsNullOrWhiteSpace($body)) {
    throw "Changelog section for tag '$Tag' is empty."
}

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    Write-Output $body
    exit 0
}

$resolvedOutputPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputPath)
$outputDir = Split-Path -Parent $resolvedOutputPath
if (-not [string]::IsNullOrWhiteSpace($outputDir)) {
    New-Item -ItemType Directory -Force $outputDir | Out-Null
}

[System.IO.File]::WriteAllText(
    $resolvedOutputPath,
    $body + [Environment]::NewLine,
    [System.Text.UTF8Encoding]::new($false))
