[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$MaterialDirectory = (Join-Path $PSScriptRoot 'MaterialWorkshop/Assets/materials'),
    [switch]$IncludeHeroes,
    [switch]$IncludeAllConditions,
    [switch]$IncludeMotion,
    [switch]$D3D12
)

$ErrorActionPreference = 'Stop'
$viewer = (Resolve-Path -LiteralPath $Executable).Path
$materials = (Resolve-Path -LiteralPath $MaterialDirectory).Path
$destination = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $destination) {
    throw 'Choose a fresh output directory; existing captures are preserved.'
}
$null = New-Item -ItemType Directory -Path $destination
$chapters = [Collections.Generic.List[object]]::new()
foreach ($scene in @('gallery', 'facade', 'workbench', 'study', 'cutout', 'shapes', 'collection', 'shapes-extra', 'transparency')) {
    $chapters.Add(@{ Name=('01-' + $scene); Scene=$scene; Variant='base'; Lighting='studio' })
}
if ($IncludeHeroes) {
    foreach ($material in @('metal', 'stone', 'wood', 'ceramic', 'rubber', 'paint', 'fabric', 'panel',
                            'copper', 'concrete', 'terrazzo', 'leather', 'asphalt', 'brick', 'marble', 'plastic', 'acrylic')) {
        $chapters.Add(@{ Name=('02-hero-' + $material); Material=$material; Variant='base'; Lighting='studio' })
    }
}
if ($IncludeAllConditions) {
    foreach ($variant in @('clean', 'wet', 'worn')) {
        foreach ($scene in @('gallery', 'facade', 'workbench', 'study', 'cutout', 'shapes', 'collection', 'shapes-extra', 'transparency')) {
            $chapters.Add(@{ Name=('03-' + $variant + '-' + $scene); Scene=$scene; Variant=$variant; Lighting='studio' })
        }
    }
}
foreach ($lighting in @('sunset', 'inspection')) {
    foreach ($scene in @('facade', 'workbench')) {
        $chapters.Add(@{ Name=('04-' + $lighting + '-' + $scene); Scene=$scene; Variant='base'; Lighting=$lighting })
    }
}
if ($IncludeMotion) {
    $chapters.Add(@{ Name='05-surface-motion'; Scene='gallery'; Variant='base'; Lighting='studio'; Motion=$true })
    $chapters.Add(@{ Name='05-cutout-motion'; Scene='cutout'; Variant='base'; Lighting='inspection'; Motion=$true })
    $chapters.Add(@{ Name='05-light-sweep'; Material='metal'; Variant='wet'; Lighting='studio'; Motion=$true; Sweep=$true })
}

# Record exact source identities without modifying the sample project or its maps.
function Get-MaterialSourceIdentities {
    Get-ChildItem -LiteralPath $materials -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path=[IO.Path]::GetRelativePath($materials, $_.FullName).Replace('\', '/')
            sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
}
$sources = @(Get-MaterialSourceIdentities)
$sourceIdentity = ConvertTo-Json -InputObject $sources -Depth 4 -Compress
$viewerIdentity = (Get-FileHash -LiteralPath $viewer -Algorithm SHA256).Hash.ToLowerInvariant()
Copy-Item -LiteralPath (Join-Path $materials 'library.json') -Destination (Join-Path $destination 'source-library.json')
Copy-Item -LiteralPath (Join-Path $materials 'pbr.phoskia') -Destination (Join-Path $destination 'source-pbr.phoskia')
$receipts = [Collections.Generic.List[object]]::new()
foreach ($chapter in $chapters) {
    $folder = Join-Path $destination $chapter.Name
    $null = New-Item -ItemType Directory -Path $folder
    $arguments = @('--hidden', '--materials', $materials, '--output', $folder,
                   '--variant', $chapter.Variant, '--lighting', $chapter.Lighting)
    if ($chapter.Scene) { $arguments += @('--scene', $chapter.Scene) }
    if ($chapter.Material) { $arguments += @('--material', $chapter.Material) }
    if ($chapter.Motion) { $arguments += '--motion-gallery' } else { $arguments += '--gallery' }
    if ($chapter.Sweep) { $arguments += @('--lighting-sweep', '--freeze-surface') }
    if ($D3D12) { $arguments += '--d3d12' }
    Write-Host ('Exporting ' + $chapter.Name)
    & $viewer @arguments *> (Join-Path $destination ($chapter.Name + '.log'))
    if ($LASTEXITCODE -ne 0) {
        throw "Showcase stopped at $($chapter.Name), exit $LASTEXITCODE; completed output is preserved."
    }
    $currentIdentity = ConvertTo-Json -InputObject @(Get-MaterialSourceIdentities) -Depth 4 -Compress
    if ($currentIdentity -cne $sourceIdentity) {
        throw "Source materials changed during $($chapter.Name); captures are preserved without accepting that chapter."
    }
    $images = @(Get-ChildItem -LiteralPath $folder -File -Filter '*.png')
    $expected = if ($chapter.Motion) { 64 } else { 2 }
    if ($images.Count -ne $expected) {
        throw "Expected $expected completed images in $($chapter.Name), found $($images.Count)."
    }
    $receipts.Add([ordered]@{
        name=$chapter.Name; scene=$chapter.Scene; material=$chapter.Material
        variant=$chapter.Variant; lighting=$chapter.Lighting
        motion=[bool]$chapter.Motion; lightingSweep=[bool]$chapter.Sweep
        images=@($images | Sort-Object Name | Select-Object -ExpandProperty Name)
    })
    [ordered]@{
        schemaVersion=1; backend=$(if($D3D12){'D3D12'}else{'D3D11'})
        viewerSha256=$viewerIdentity
        sourceFiles=$sources; completedChapters=$receipts.ToArray()
        motionSubmissionRate=30; motionCaptureStride=3
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $destination 'showcase.json') -Encoding utf8
}
Write-Host ('Material showcase saved: ' + $destination)
