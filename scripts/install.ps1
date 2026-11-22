# Journey Coop - instalador
# Uso: clique duas vezes em install.cmd, ou:
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-Room CODIGO] [-Partner STEAMID64] [-GamePath PASTA] [-NoVarsTweak]
param(
    [string]$Room,
    [string]$Partner,
    [string]$GamePath,
    [switch]$NoVarsTweak
)

. (Join-Path $PSScriptRoot 'common.ps1')

try {
    Write-Host ''
    Write-Host '  Journey Coop - instalador' -ForegroundColor White
    Write-Host ''

    $modDll = Join-Path $PSScriptRoot 'steam_api64.dll'
    if (-not (Test-Path $modDll)) { throw "steam_api64.dll do mod nao esta ao lado deste script ($PSScriptRoot)." }
    if (-not (Test-IsCoopDll $modDll)) { throw 'O steam_api64.dll ao lado do script nao e o do mod.' }

    Write-Step 'Procurando o Journey'
    $game = Find-JourneyPath $GamePath
    Write-Ok $game
    Assert-GameClosed

    $dll     = Join-Path $game 'steam_api64.dll'
    $orig    = Join-Path $game 'steam_api64_o.dll'
    $backup  = Join-Path $game 'journey-coop-backup'
    $ini     = Join-Path $game 'journey-coop.ini'
    New-Item -ItemType Directory -Force $backup | Out-Null

    Write-Step 'Guardando a DLL original da Steam'
    if (Test-IsCoopDll $dll) {
        if (-not (Test-Path $orig)) { throw "steam_api64.dll ja e o mod, mas steam_api64_o.dll sumiu. Use 'Verificar integridade' na Steam e rode o instalador de novo." }
        Write-Ok 'mod ja instalado, so atualizando'
    } else {
        # dll is Steam's original (fresh install, or Steam restored it after an update/verify)
        Copy-Item $dll (Join-Path $backup 'steam_api64.dll') -Force
        Copy-Item $dll $orig -Force
        Write-Ok 'steam_api64.dll -> steam_api64_o.dll (copia extra em journey-coop-backup\)'
    }

    Write-Step 'Copiando o mod'
    Copy-Item $modDll $dll -Force
    Write-Ok 'steam_api64.dll do mod instalado'

    Write-Step 'Configurando o codigo da sala'
    $curRoom = ''; $curPartner = '0'
    if (Test-Path $ini) {
        $c = Get-Content $ini -Raw
        if ($c -match '(?m)^\s*room\s*=\s*(\S*)') { $curRoom = $Matches[1] }
        if ($c -match '(?m)^\s*partner\s*=\s*(\S*)') { $curPartner = $Matches[1] }
    }
    if (-not $Room) {
        $hint = if ($curRoom) { " [Enter = manter $curRoom]" } else { '' }
        $Room = Read-Host "Codigo da sala (letras/numeros, o MESMO nos dois PCs)$hint"
        if (-not $Room) { $Room = $curRoom }
    }
    $Room = ($Room -replace '[^A-Za-z0-9]', '').ToUpper()
    if ($Room.Length -lt 4) { throw 'Codigo da sala precisa ter pelo menos 4 letras/numeros.' }
    if ($Room.Length -gt 32) { $Room = $Room.Substring(0, 32) }
    if (-not $Partner) { $Partner = $curPartner }
    if ($Partner -notmatch '^\d+$') { throw 'partner precisa ser o SteamID64 (so numeros) ou 0.' }

    $iniText = @"
; Journey Coop - configuracao
[coop]
; Codigo da sala: tem que ser IGUAL nos dois PCs. Vazio = mod desligado.
room=$Room
; (Opcional) SteamID64 do parceiro. 0 = aceitar qualquer um com o mesmo codigo.
partner=$Partner
; 1 = grava journey-coop.log na pasta do jogo (ajuda a diagnosticar)
log=1
"@
    Set-Content -Path $ini -Value $iniText -Encoding ASCII
    Write-Ok "room=$Room  partner=$Partner"

    if (-not $NoVarsTweak) {
        Write-Step 'Aumentando o raio de encontro (Data\Scripts\Vars.lua)'
        $vars = Join-Path $game 'Data\Scripts\Vars.lua'
        $varsBak = Join-Path $backup 'Vars.lua'
        if (Test-Path $vars) {
            if (-not (Test-Path $varsBak)) { Copy-Item $vars $varsBak }
            $t = [System.IO.File]::ReadAllText($vars)
            $n = $t
            $n = [regex]::Replace($n, '(kJoinRadiusFar\s*=\s*)[\d.]+', '${1}5000')
            $n = [regex]::Replace($n, '(kJoinRadiusNear\s*=\s*)[\d.]+', '${1}5000')
            $n = [regex]::Replace($n, '(kLeaveRadius\s*=\s*)[\d.]+', '${1}10000')
            if ($n -ne $t) { [System.IO.File]::WriteAllText($vars, $n); Write-Ok 'raios aumentados (backup em journey-coop-backup\Vars.lua)' }
            else { Write-Ok 'ja estava ajustado' }
        } else { Write-Warn2 'Vars.lua nao encontrado, pulando' }
    }

    Write-Host ''
    Write-Host '  Instalado! Abra o Journey pela Steam normalmente.' -ForegroundColor Green
    Write-Host "  Codigo da sala: $Room  (o seu amigo tem que usar o mesmo)" -ForegroundColor Green
    Write-Host ''
} catch {
    Write-Host ''
    Write-Host "  ERRO: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host ''
    exit 1
}
