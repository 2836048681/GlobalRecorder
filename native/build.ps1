param(
    [string]$ZigPath = '',
    [string]$OutputName = 'GlobalRecorder.exe'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if ([IO.Path]::GetFileName($OutputName) -ne $OutputName -or
    [IO.Path]::GetExtension($OutputName) -ne '.exe') {
    throw 'OutputName must be an exe filename.'
}
if (-not $ZigPath) {
    $zigCommand = Get-Command zig -ErrorAction SilentlyContinue
    if ($zigCommand) { $ZigPath = $zigCommand.Source }
}
if (-not $ZigPath -or -not (Test-Path -LiteralPath $ZigPath -PathType Leaf)) {
    throw 'Zig 0.15.2 is required. Add zig to PATH or pass -ZigPath with its full path.'
}
$version = & $ZigPath version
if ($LASTEXITCODE -ne 0 -or $version.Trim() -ne '0.15.2') {
    throw 'This project is validated with Zig 0.15.2.'
}
$buildDir = Join-Path $projectRoot 'native-bin'
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
$arguments = @(
    'c++', '-std=c++17', '-O2', '-DNDEBUG', '-DUNICODE', '-D_UNICODE',
    '-fms-extensions', '-municode', '-Wl,--subsystem,windows', '-static', '-s',
    (Join-Path $PSScriptRoot 'main.cpp'),
    (Join-Path $PSScriptRoot 'audio.cpp'),
    (Join-Path $PSScriptRoot 'shell_support.cpp'),
    '-o', (Join-Path $buildDir $OutputName),
    '-lole32', '-luuid', '-luser32', '-lgdi32', '-lgdiplus',
    ('-l' + 'com' + 'ctl32'),
    '-lwinmm', '-lshlwapi', '-lpsapi', '-ldwmapi',
    ('-l' + 'adv' + 'api32')
)
& $ZigPath @arguments
if ($LASTEXITCODE -ne 0) { throw 'Native build failed.' }
Get-Item -LiteralPath (Join-Path $buildDir $OutputName) | Select-Object FullName,Length
