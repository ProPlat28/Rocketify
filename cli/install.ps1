# Installs the rocketify command in %USERPROFILE%\.rocketify\bin.
$ErrorActionPreference = 'Stop'
$bin = Join-Path $env:USERPROFILE '.rocketify\bin'
New-Item -ItemType Directory -Force $bin | Out-Null
Invoke-WebRequest -UseBasicParsing 'https://raw.githubusercontent.com/ProPlat28/Rocketify/refs/heads/main/dist/cli/rocketify.exe' -OutFile (Join-Path $bin 'rocketify.exe')
$path = [Environment]::GetEnvironmentVariable('Path', 'User')
if (($path -split ';') -notcontains $bin) {
    [Environment]::SetEnvironmentVariable('Path', (@($path, $bin) | Where-Object { $_ }) -join ';', 'User')
}
Write-Host 'Installed. Open a new terminal, then run: rocketify install dependencies'
