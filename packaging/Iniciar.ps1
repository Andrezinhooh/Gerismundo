[CmdletBinding()]
param()

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$packageRoot = $PSScriptRoot
$clientExe = Join-Path $packageRoot 'native-call.exe'
$serverExe = Join-Path $packageRoot 'native-call-signaling.exe'
$configPath = Join-Path $packageRoot 'config.json'
$hostConfigPath = Join-Path $packageRoot 'host-config.json'
$serverStatePath = Join-Path $packageRoot 'server-state.json'
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Read-JsonFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    return ([System.IO.File]::ReadAllText($Path) | ConvertFrom-Json)
}

function Write-JsonFile([string]$Path, $Value) {
    [System.IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 8), $utf8)
}

function Read-Value([string]$Prompt, [string]$Default) {
    $label = $Prompt
    if ($Default) { $label += " [$Default]" }
    $value = Read-Host $label
    if ([string]::IsNullOrWhiteSpace($value)) { return $Default }
    return $value.Trim()
}

function Validate-Label([string]$Value, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($Value) -or $utf8.GetByteCount($Value) -gt 64 -or $Value -match '[\x00-\x1f\x7f]') {
        throw "$Label deve conter de 1 a 64 bytes UTF-8, sem caracteres de controle."
    }
}

function New-Token {
    $bytes = New-Object byte[] 32
    $generator = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try { $generator.GetBytes($bytes) } finally { $generator.Dispose() }
    return [Convert]::ToBase64String($bytes)
}

function Get-ListenerProcessId([int]$Port) {
    $listeners = @(Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue)
    return @($listeners | Select-Object -ExpandProperty OwningProcess -Unique)
}

function Ensure-Server([string]$BindAddress, [string]$Token) {
    $port = 8080
    $state = Read-JsonFile $serverStatePath
    $ownedProcess = $null
    if ($state) {
        $candidate = Get-Process -Id ([int]$state.ProcessId) -ErrorAction SilentlyContinue
        if ($candidate) {
            try {
                $samePath = [string]::Equals($candidate.Path, $serverExe, [StringComparison]::OrdinalIgnoreCase)
                $sameStart = $candidate.StartTime.ToUniversalTime().Ticks -eq ([DateTime]::Parse($state.StartedAt)).ToUniversalTime().Ticks
                if ($samePath -and $sameStart) { $ownedProcess = $candidate }
            } catch { $ownedProcess = $null }
        }
    }
    $listeners = @(Get-ListenerProcessId $port)
    if ($ownedProcess) {
        if ($state.BindAddress -ne $BindAddress) {
            throw 'Ja existe um servidor desta pasta em outro modo. Feche a janela do servidor antes de trocar entre Local e LAN.'
        }
        if ($listeners -contains $ownedProcess.Id) {
            Write-Host 'Reutilizando o servidor desta pasta.'
            return
        }
        throw 'O servidor desta pasta ainda esta ativo, mas nao esta ouvindo na porta 8080. Feche sua janela e tente novamente.'
    }
    if ($listeners.Count -gt 0) {
        throw 'A porta 8080 esta ocupada por outro processo. Nenhum processo foi encerrado. Feche-o ou escolha Entrar em um servidor existente.'
    }

    $previousToken = [Environment]::GetEnvironmentVariable('NATIVE_CALL_TOKEN', 'Process')
    try {
        [Environment]::SetEnvironmentVariable('NATIVE_CALL_TOKEN', $Token, 'Process')
        $server = Start-Process -FilePath $serverExe -WorkingDirectory $packageRoot -ArgumentList @('--bind', $BindAddress, '--port', '8080') -PassThru
    } finally {
        [Environment]::SetEnvironmentVariable('NATIVE_CALL_TOKEN', $previousToken, 'Process')
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($server.HasExited) { throw "O servidor encerrou antes de iniciar (codigo $($server.ExitCode))." }
        if (@(Get-ListenerProcessId $port) -contains $server.Id) {
            Write-JsonFile $serverStatePath ([ordered]@{
                ProcessId = $server.Id
                StartedAt = $server.StartTime.ToUniversalTime().ToString('o')
                BindAddress = $BindAddress
                Port = $port
            })
            return
        }
        Start-Sleep -Milliseconds 200
    }
    if (-not $server.HasExited) { $server.Kill(); $server.WaitForExit() }
    throw 'O servidor desta pasta nao conseguiu iniciar na porta 8080 em 15 segundos.'
}

try {
    foreach ($exe in @($clientExe, $serverExe)) {
        if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
            throw "Executavel ausente: $exe. Extraia todo o pacote ZIP."
        }
    }
    Write-Host 'Gerismundo - chamadas e compartilhamento para Windows' -ForegroundColor Cyan
    Write-Host '1 - Testar neste computador (servidor somente local)'
    Write-Host '2 - Hospedar para computadores da mesma rede local (LAN)'
    Write-Host '3 - Entrar em um servidor existente'
    Write-Host '4 - Abrir com a configuracao salva'
    $choice = Read-Value 'Escolha' '1'
    if ($choice -notin @('1', '2', '3', '4')) { throw 'Escolha uma opcao entre 1 e 4.' }

    $existing = Read-JsonFile $configPath
    if ($choice -eq '4') {
        if (-not $existing) { throw 'Ainda nao existe config.json. Inicie novamente e escolha 1, 2 ou 3.' }
    } else {
        $nameDefault = $env:USERNAME
        if ($existing -and $existing.displayName) { $nameDefault = [string]$existing.displayName }
        $name = Read-Value 'Seu nome' $nameDefault
        $room = Read-Value 'Sala' 'amigos'
        Validate-Label $name 'Nome'
        Validate-Label $room 'Sala'
        $iceServers = @()
        if ($existing -and $existing.iceServers) {
            $iceServers = @($existing.iceServers | Where-Object { $_ -and $_.url })
        }
        if ($choice -eq '3') {
            $serverUrl = Read-Value 'URL do servidor (ws://IP:8080 ou wss://dominio)' ''
            if ($serverUrl -notmatch '^wss?://\S+$') { throw 'Informe uma URL ws:// ou wss:// valida.' }
            $token = Read-Host 'Token fornecido por quem hospeda o servidor'
            if ($utf8.GetByteCount($token) -lt 24 -or $utf8.GetByteCount($token) -gt 256) {
                throw 'O token deve ter de 24 a 256 bytes e ser o mesmo usado pelo servidor.'
            }
            if ($iceServers.Count -eq 0) { $iceServers = @(@{ url = 'stun:stun.l.google.com:19302' }) }
        } else {
            $hostConfig = Read-JsonFile $hostConfigPath
            if ($hostConfig -and $hostConfig.accessToken) { $token = [string]$hostConfig.accessToken }
            else {
                $token = New-Token
                Write-JsonFile $hostConfigPath ([ordered]@{ accessToken = $token })
            }
            $bind = '127.0.0.1'
            if ($choice -eq '2') { $bind = '0.0.0.0' }
            Ensure-Server $bind $token
            $serverUrl = 'ws://127.0.0.1:8080'
            if ($choice -eq '2') {
                Write-Host ''
                Write-Host 'Compartilhe estes dados somente com os participantes da sua rede:' -ForegroundColor Yellow
                $addresses = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue | Where-Object {
                    $_.IPAddress -ne '127.0.0.1' -and $_.IPAddress -notlike '169.254.*' -and $_.AddressState -eq 'Preferred'
                } | Select-Object -ExpandProperty IPAddress -Unique)
                foreach ($address in $addresses) { Write-Host "Servidor: ws://${address}:8080" }
                Write-Host "Sala: $room"
                Write-Host "Token: $token"
                Write-Host 'O Windows pode solicitar acesso de rede ao servidor e ao cliente. Nenhuma regra de firewall foi alterada pelo assistente.'
                Write-Host 'Use WSS e TURN para uso pela internet; o modo LAN nao publica um servidor na internet.'
                Write-Host ''
            }
        }
        Write-JsonFile $configPath ([ordered]@{
            signalingUrl = $serverUrl
            room = $room
            displayName = $name
            accessToken = $token
            iceServers = @($iceServers)
        })
    }
    Write-Host 'Abrindo o aplicativo. Clique em Entrar na interface.'
    Start-Process -FilePath $clientExe -WorkingDirectory $packageRoot -ArgumentList @('--config', ('"' + $configPath + '"')) | Out-Null
} catch {
    Write-Host ('Nao foi possivel iniciar: ' + $_.Exception.Message) -ForegroundColor Red
    exit 1
}
