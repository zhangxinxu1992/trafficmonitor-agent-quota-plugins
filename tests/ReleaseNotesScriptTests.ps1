Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Equal {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Actual,
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string]$Expected,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    if ($Actual -ne $Expected) {
        throw "$Message`nExpected:`n$Expected`nActual:`n$Actual"
    }
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$scriptPath = Join-Path $repoRoot '.github\scripts\Get-ReleaseNotes.ps1'
$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("trafficmonitor-release-notes-test-" + [System.Guid]::NewGuid())

New-Item -ItemType Directory -Path $tempDir | Out-Null

try {
    $changelogPath = Join-Path $tempDir 'CHANGELOG.md'
    $outputPath = Join-Path $tempDir 'notes.md'
    [System.IO.File]::WriteAllText(
        $changelogPath,
        @'
# Changelog

## v1.1

### Added

- New feature.

### Fixed

- Bug fix.

## v1.0

### Added

- Initial release.
'@,
        [System.Text.UTF8Encoding]::new($false))

    & $scriptPath -Tag 'v1.1' -ChangelogPath $changelogPath -OutputPath $outputPath

    $actual = [System.IO.File]::ReadAllText($outputPath).Trim()
    $expected = @'
### Added

- New feature.

### Fixed

- Bug fix.
'@.Trim()

    Assert-Equal -Actual $actual -Expected $expected -Message 'The extractor should write only the requested release body.'

    $stdout = (& $scriptPath -Tag 'v1.1' -ChangelogPath $changelogPath | Out-String).Trim()
    Assert-Equal -Actual $stdout -Expected $expected -Message 'The extractor should write capturable pipeline output when no output path is supplied.'

    $missingError = $null
    try {
        & $scriptPath -Tag 'v9.9' -ChangelogPath $changelogPath -OutputPath (Join-Path $tempDir 'missing.md')
    }
    catch {
        $missingError = $_.Exception.Message
    }

    if ([string]::IsNullOrWhiteSpace($missingError)) {
        throw 'The extractor should fail when the requested tag is missing.'
    }

    if ($missingError -notmatch 'No changelog section found') {
        throw "Missing tag failure should explain the missing changelog section. Actual: $missingError"
    }
}
finally {
    Remove-Item -Recurse -Force $tempDir
}

Write-Output 'Release notes script tests passed.'
