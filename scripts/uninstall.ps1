# Journey Coop - desinstalador
# Uso: clique duas vezes em uninstall.cmd, ou:
#   powershell -ExecutionPolicy Bypass -File uninstall.ps1 [-GamePath PASTA]
param([string]$GamePath)

. (Join-Path $PSScriptRoot 'common.ps1')

try {
    Write-Host ''
    Write-Host '  Journey Coop - desinstalador' -ForegroundColor White
    Write-Host ''

    Write-Step 'Procurando o Journey'
    $game = Find-JourneyPath $GamePath
    Write-Ok $game
    Assert-GameClosed

    $dll    = Join-Path $game 'steam_api64.dll'
    $orig   = Join-Path $game 'steam_api64_o.dll'
    $backup = Join-Path $game 'journey-coop-backup'
    $needVerify = $false

    Write-Step 'Restaurando a DLL original da Steam'
    if (Test-IsCoopDll $dll) {
        $src = if (Test-Path $orig) { $orig } elseif (Test-Path (Join-Path $backup 'steam_api64.dll')) { Join-Path $backup 'steam_api64.dll' } else { $null }
        if ($src) {
            Copy-Item $src $dll -Force
            Write-Ok 'steam_api64.dll original de volta'
        } else {
            Remove-Item $dll -Force
            $needVerify = $true
            Write-Warn2 'original nao encontrada; DLL do mod removida'
        }
    } else {
        Write-Ok 'steam_api64.dll ja e a original'
    }
    if (Test-Path $orig) { Remove-Item $orig -Force }

    Write-Step 'Restaurando Vars.lua'
    $varsBak = Join-Path $backup 'Vars.lua'
    if (Test-Path $varsBak) {
        Copy-Item $varsBak (Join-Path $game 'Data\Scripts\Vars.lua') -Force
        Write-Ok 'Vars.lua original de volta'
    } else { Write-Ok 'nada para restaurar' }

    Write-Step 'Limpando arquivos do mod'
    foreach ($f in 'journey-coop.ini', 'journey-coop.log') {
        $p = Join-Path $game $f
        if (Test-Path $p) { Remove-Item $p -Force }
    }
    if (Test-Path $backup) { Remove-Item $backup -Recurse -Force }
    Write-Ok 'ini, log e backup removidos'

    Write-Host ''
    if ($needVerify) {
        Write-Host '  Quase pronto: na Steam, clique com o botao direito em Journey > Propriedades >' -ForegroundColor Yellow
        Write-Host '  Arquivos instalados > Verificar integridade dos arquivos.' -ForegroundColor Yellow
    } else {
        Write-Host '  Desinstalado. O Journey esta como veio da Steam.' -ForegroundColor Green
    }
    Write-Host ''
} catch {
    Write-Host ''
    Write-Host "  ERRO: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host ''
    exit 1
}
