# Shared helpers for install.ps1 / uninstall.ps1 (ASCII only: Windows PowerShell 5.1)

$ErrorActionPreference = 'Stop'

function Write-Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg)   { Write-Host "    OK: $msg" -ForegroundColor Green }
function Write-Warn2($msg){ Write-Host "    AVISO: $msg" -ForegroundColor Yellow }

function Find-JourneyPath {
    param([string]$Given)
    if ($Given) {
        if (Test-Path (Join-Path $Given 'Journey.exe')) { return (Resolve-Path $Given).Path }
        throw "Journey.exe nao encontrado em '$Given'."
    }
    $candidates = @()
    try {
        $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath
        if ($steam) {
            $steam = $steam -replace '/', '\'
            $candidates += (Join-Path $steam 'steamapps\common\Journey')
            $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
            if (Test-Path $vdf) {
                foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                    $lib = $m.Groups[1].Value -replace '\\\\', '\'
                    $candidates += (Join-Path $lib 'steamapps\common\Journey')
                }
            }
        }
    } catch { }
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c 'Journey.exe')) { return $c }
    }
    $p = Read-Host 'Nao achei o Journey automaticamente. Cole a pasta do jogo (onde fica Journey.exe)'
    $p = $p.Trim('"', ' ')
    if (Test-Path (Join-Path $p 'Journey.exe')) { return $p }
    throw "Journey.exe nao encontrado em '$p'."
}

function Test-IsCoopDll([string]$path) {
    if (-not (Test-Path $path)) { return $false }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $text = [System.Text.Encoding]::ASCII.GetString($bytes)
    return $text.Contains('Journey Coop proxy loaded')
}

function Assert-GameClosed {
    if (Get-Process -Name 'Journey' -ErrorAction SilentlyContinue) {
        throw 'O Journey esta aberto. Feche o jogo e rode de novo.'
    }
}
