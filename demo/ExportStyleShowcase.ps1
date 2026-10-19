[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [switch]$IncludeMotion,
    [switch]$D3D12
)

$ErrorActionPreference = 'Stop'
$demoExecutable = (Resolve-Path -LiteralPath $Executable).Path
if (-not (Test-Path -LiteralPath $demoExecutable -PathType Leaf)) {
    throw 'Executable must name the built AYRenderer_ColorStylizationDemo.'
}
$showcaseRoot = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $showcaseRoot) {
    throw 'Choose a fresh output directory; existing captures are preserved.'
}
$null = New-Item -ItemType Directory -Path $showcaseRoot
$chapters = @(
    @{ Name='01-original-exhibit'; Scene='gallery'; Look='0.rsl' },
    @{ Name='02-sunset-canal'; Scene='canal'; Look='5.rsl' },
    @{ Name='03-blueprint-workshop'; Scene='workshop'; Look='9.rsl' },
    @{ Name='04-cel-observatory'; Scene='observatory'; Look='cel-ink.rsl' },
    @{ Name='05-paper-city'; Scene='city'; Look='paper-echo.rsl' },
    @{ Name='06-phosphor-character'; Scene='character'; Look='phosphor-stop.rsl' }
)
if ($IncludeMotion) {
    $chapters += @(
        @{ Name='07-motion-original'; Scene='character'; Look='0.rsl'; Motion=$true },
        @{ Name='08-motion-echo'; Scene='character'; Look='time-echo.rsl'; Motion=$true },
        @{ Name='09-motion-paper'; Scene='character'; Look='paper-echo.rsl'; Motion=$true },
        @{ Name='10-motion-phosphor'; Scene='character'; Look='phosphor-stop.rsl'; Motion=$true },
        @{ Name='11-palette-performance'; Scene='observatory'; Look='reference-palette.rsl'; Motion=$true; Palette=$true }
    )
}
$receipts = [Collections.Generic.List[object]]::new()
foreach ($chapter in $chapters) {
    $look = Join-Path $PSScriptRoot ('looks/' + $chapter.Look)
    $destination = Join-Path $showcaseRoot $chapter.Name
    $null = New-Item -ItemType Directory -Path $destination
    $snapshot = Join-Path $destination 'source-look.rsl'
    Copy-Item -LiteralPath $look -Destination $snapshot
    $arguments = @('--hidden', '--scene', $chapter.Scene, '--look', $snapshot, '--output', $destination)
    if ($chapter.Motion) { $arguments += '--motion-gallery' } else { $arguments += '--gallery' }
    if ($chapter.Palette) { $arguments += '--animate-palette' }
    if ($D3D12) { $arguments += '--d3d12' }
    Write-Host ('Exporting ' + $chapter.Name)
    & $demoExecutable @arguments *> (Join-Path $showcaseRoot ($chapter.Name + '.log'))
    if ($LASTEXITCODE -ne 0) {
        throw "Showcase stopped at $($chapter.Name), exit $LASTEXITCODE; completed chapters and logs are preserved."
    }
    $receipts.Add([ordered]@{
        name=$chapter.Name; scene=$chapter.Scene; look=$chapter.Look
        snapshot=($chapter.Name + '/source-look.rsl')
        lookSha256=(Get-FileHash -LiteralPath $snapshot -Algorithm SHA256).Hash.ToLowerInvariant()
        motion=[bool]$chapter.Motion; palettePerformance=[bool]$chapter.Palette
        output=$destination; images=@(Get-ChildItem -LiteralPath $destination -Filter '*.png' -File).Count
    })
    [ordered]@{
        schemaVersion=1; backend=$(if($D3D12){'D3D12'}else{'D3D11'})
        completedChapters=$receipts.ToArray()
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $showcaseRoot 'showcase.json') -Encoding utf8
}
Write-Host ('Showcase saved: ' + $showcaseRoot)
