[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/windows/Release',
    [string]$OutputDirectory = 'out',
    [string]$TestReportPath = ''
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$sourceRoot = Split-Path -Parent $PSScriptRoot
function Absolute-Path([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
    return [System.IO.Path]::GetFullPath((Join-Path $sourceRoot $Path))
}
$buildRoot = Absolute-Path $BuildDirectory
$outputRoot = Absolute-Path $OutputDirectory
$folderName = 'Gerismundo-Windows-x64'
$stage = Join-Path $outputRoot $folderName
$archive = Join-Path $outputRoot ($folderName + '.zip')
$utf8 = New-Object System.Text.UTF8Encoding($false)

foreach ($required in @('native-call.exe', 'native-call-signaling.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $buildRoot $required) -PathType Leaf)) {
        throw "Required executable missing: $required in $buildRoot"
    }
}
if ($stage -eq $sourceRoot -or $stage -eq $buildRoot) { throw 'Package output overlaps input.' }
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null
Get-ChildItem -LiteralPath $buildRoot -File | Where-Object { $_.Extension -in @('.exe', '.dll') } | Copy-Item -Destination $stage
foreach ($name in @('Iniciar.cmd', 'Iniciar.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $stage
}
foreach ($name in @('README.md', 'THIRD_PARTY.md', 'config.example.json', 'config.local.example.json')) {
    Copy-Item -LiteralPath (Join-Path $sourceRoot $name) -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $sourceRoot 'docs') -Destination $stage -Recurse
Copy-Item -LiteralPath (Join-Path $sourceRoot 'deploy') -Destination $stage -Recurse
Copy-Item -LiteralPath (Join-Path $sourceRoot 'docs/COMO_USAR.md') -Destination (Join-Path $stage 'LEIA-ME.md')

# Include the license/copyright material actually installed by vcpkg.
$licenses = Join-Path $stage 'licencas'
New-Item -ItemType Directory -Path $licenses | Out-Null
$installedRoots = @((Join-Path $sourceRoot 'vcpkg_installed'), (Join-Path (Split-Path -Parent $buildRoot) 'vcpkg_installed'))
foreach ($installedRoot in $installedRoots) {
    if (-not (Test-Path -LiteralPath $installedRoot)) { continue }
    Get-ChildItem -LiteralPath $installedRoot -Filter copyright -File -Recurse | ForEach-Object {
        $packageName = Split-Path -Leaf (Split-Path -Parent $_.FullName)
        $tripletName = Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $_.FullName)))
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $licenses ($tripletName + '-' + $packageName + '.txt'))
    }
}
if (@(Get-ChildItem -LiteralPath $licenses -File).Count -eq 0) {
    throw 'No installed dependency copyright files found; refusing an incomplete distribution.'
}

if ($TestReportPath) {
    $testReport = Absolute-Path $TestReportPath
    if (-not (Test-Path -LiteralPath $testReport -PathType Leaf)) { throw "Test report not found: $testReport" }
    Copy-Item -LiteralPath $testReport -Destination (Join-Path $stage 'TESTES-AUTOMATIZADOS.txt')
}

# Archive the current source, including dotfiles, without generated binaries,
# local credentials, git internals, old dist archives or downloaded toolchains.
$sourceArchive = Join-Path $stage 'codigo-fonte.zip'
$zip = [System.IO.Compression.ZipFile]::Open($sourceArchive, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    $roots = @('.github', 'src', 'include', 'tests', 'docs', 'deploy', 'packaging')
    $rootFiles = @('.gitignore', 'README.md', 'THIRD_PARTY.md', 'CMakeLists.txt', 'CMakePresets.json', 'vcpkg.json', 'app.manifest', 'config.example.json', 'config.local.example.json')
    $sourceFiles = @()
    foreach ($name in $roots) {
        $path = Join-Path $sourceRoot $name
        if (Test-Path -LiteralPath $path) {
            $sourceFiles += @(Get-ChildItem -LiteralPath $path -File -Recurse -Force | Where-Object { $_.FullName -notmatch '[\\/]__pycache__[\\/]' })
        }
    }
    foreach ($name in $rootFiles) {
        $path = Join-Path $sourceRoot $name
        if (Test-Path -LiteralPath $path) { $sourceFiles += Get-Item -LiteralPath $path -Force }
    }
    foreach ($file in ($sourceFiles | Sort-Object FullName)) {
        $relative = $file.FullName.Substring($sourceRoot.Length).TrimStart('\', '/').Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, ('native-call/' + $relative), [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $zip.Dispose() }

$records = @(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{
        path = $_.FullName.Substring($stage.Length).TrimStart('\', '/').Replace('\', '/')
        size = $_.Length
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
$commit = 'unknown'
if ($env:GITHUB_SHA) { $commit = $env:GITHUB_SHA }
$manifest = [ordered]@{
    product = 'Gerismundo'
    platform = 'Windows x64'
    sourceCommit = $commit
    builtAtUtc = [DateTime]::UtcNow.ToString('o')
    runtime = 'Static C++ runtime and dependencies; Windows system components required'
    validation = 'See TESTES-AUTOMATIZADOS.txt and docs/TESTES.md; physical microphone/screen/internet tests are separate'
    files = $records
}
[System.IO.File]::WriteAllText((Join-Path $stage 'manifest.json'), ($manifest | ConvertTo-Json -Depth 8), $utf8)
if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -Force }
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $archive, [System.IO.Compression.CompressionLevel]::Optimal, $true)
$checksum = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
[System.IO.File]::WriteAllText(($archive + '.sha256'), ($checksum + '  ' + [System.IO.Path]::GetFileName($archive) + "`n"), $utf8)
Write-Host "Portable package: $archive"
Write-Host "SHA256: $checksum"
