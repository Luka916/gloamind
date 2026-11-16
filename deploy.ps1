<#
    deploy.ps1 - one command to build and flash the GloaMind eyes.

    Written for people who have never used Arduino IDE, a compiler, or a terminal.
    Everything it changes lives in your normal Arduino folders; nothing is
    downloaded twice if you already have it.

        powershell -ExecutionPolicy Bypass -File deploy.ps1 doctor
        powershell -ExecutionPolicy Bypass -File deploy.ps1 setup
        powershell -ExecutionPolicy Bypass -File deploy.ps1 flash
        powershell -ExecutionPolicy Bypass -File deploy.ps1 all

    doctor  look, do not touch. says what is missing.
    setup   install arduino-cli, the ESP32 core, TFT_eSPI, and the panel config.
    flash   compile and upload to the board that is plugged in.
    all     setup, then flash.
#>

[CmdletBinding()]
param(
    [ValidateSet('doctor', 'setup', 'flash', 'all')]
    [string]$Mode = 'doctor',

    [string]$Port = ''
)

$ErrorActionPreference = 'Stop'

$Root     = $PSScriptRoot
$Sketch   = Join-Path $Root 'firmware\pet_eye'
$ToolsDir = Join-Path $Root '.tools'
$LocalCli = Join-Path $ToolsDir 'arduino-cli.exe'

# The Chinese mirror. The official espressif.github.io index is often unusably
# slow from mainland China, which is where this board gets built.
$IndexUrl = 'https://arduino.me/packages/esp32.json'
$CoreSpec = 'esp32:esp32'
$LibSpec  = 'TFT_eSPI'

# Every flag here matters, and getting one wrong produces a board that boots but
# shows nothing, or a compiler error nobody can read.
#   PSRAM=opi            N16R8 ships octal PSRAM; quad reports 0MB
#   FlashSize=16M        the default is 4M and the binary will not fit
#   CDCOnBoot=cdc        Serial goes over the native USB port, not UART0
#   PartitionScheme      app3M_fat9M_16MB; the default app slot is 1.25MB
$Fqbn = 'esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,CDCOnBoot=cdc,PartitionScheme=app3M_fat9M_16MB'

function Say  ($m) { Write-Host $m }
function Step ($n, $m) { Write-Host ''; Write-Host "[$n] $m" -ForegroundColor Cyan }
function Ok   ($m) { Write-Host "     OK    $m" -ForegroundColor Green }
function Warn ($m) { Write-Host "     warn  $m" -ForegroundColor Yellow }
function Die  ($m) { Write-Host "     FAIL  $m" -ForegroundColor Red; exit 1 }

function Find-Cli {
    if (Test-Path $LocalCli) { return $LocalCli }
    $cmd = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Install-Cli {
    $existing = Find-Cli
    if ($existing) { Ok "arduino-cli found: $existing"; return $existing }

    Step 1 'Downloading arduino-cli (about 12 MB)'
    if (-not (Test-Path $ToolsDir)) { New-Item -ItemType Directory -Path $ToolsDir | Out-Null }
    $zip = Join-Path $ToolsDir 'arduino-cli.zip'
    $url = 'https://downloads.arduino.cc/arduino-cli/arduino-cli_latest_Windows_64bit.zip'
    try {
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    } catch {
        Die "Could not download it. Open $url in a browser, unzip arduino-cli.exe into $ToolsDir, run this again."
    }
    Expand-Archive -Path $zip -DestinationPath $ToolsDir -Force
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
    if (-not (Test-Path $LocalCli)) {
        $found = Get-ChildItem $ToolsDir -Filter 'arduino-cli.exe' -Recurse | Select-Object -First 1
        if ($found) { Move-Item $found.FullName $LocalCli -Force }
    }
    if (-not (Test-Path $LocalCli)) { Die "Unzip finished but arduino-cli.exe is not in $ToolsDir" }
    Ok 'arduino-cli ready'
    return $LocalCli
}

function Ensure-Index ($cli) {
    Step 2 'Registering the ESP32 board index'
    $dump = (& $cli config dump --format json 2>$null) -join "`n"
    if ($dump -notmatch [regex]::Escape($IndexUrl)) {
        & $cli config add board_manager.additional_url $IndexUrl | Out-Null
        Ok 'mirror added'
    } else {
        Ok 'mirror already registered'
    }
    & $cli core update-index | Out-Null
    if ($LASTEXITCODE -ne 0) { Warn 'index update returned an error; continuing' }
}

function Ensure-Core ($cli) {
    Step 3 'Installing the ESP32 core (300-600 MB, this is the slow part)'
    $list = (& $cli core list 2>$null) -join "`n"
    if ($list -match 'esp32:esp32') {
        Ok 'already installed'
        return
    }
    & $cli core install $CoreSpec
    if ($LASTEXITCODE -ne 0) {
        Warn 'mirror install failed, trying the official index'
        & $cli config add board_manager.additional_url 'https://espressif.github.io/arduino-esp32/package_esp32_index.json' | Out-Null
        & $cli core update-index | Out-Null
        & $cli core install $CoreSpec
        if ($LASTEXITCODE -ne 0) { Die 'ESP32 core install failed. Check your network and run this again.' }
    }
    Ok 'core installed'
}

function Ensure-Lib ($cli) {
    Step 4 'Installing TFT_eSPI'
    $list = (& $cli lib list 2>$null) -join "`n"
    if ($list -match 'TFT_eSPI') {
        Ok 'already installed'
    } else {
        & $cli lib install $LibSpec
        if ($LASTEXITCODE -ne 0) { Die 'TFT_eSPI install failed.' }
        Ok 'installed'
    }
}

function Get-UserDir ($cli) {
    # `config get` returns an array with a trailing empty element, so a bare
    # .Trim() hands Join-Path an empty string. Take the first line, and fall
    # back to the documented default if the call says nothing.
    $raw = (& $cli config get directories.user 2>$null) -join "`n"
    $line = ($raw -split "`r?`n")[0]
    if ($line) { $line = $line.Trim() }
    if (-not $line) { $line = Join-Path $env:USERPROFILE 'Documents\Arduino' }
    return $line
}

function Ensure-Setup ($cli) {
    Step 5 'Writing the panel configuration'
    $userDir = Get-UserDir $cli
    $setup = Join-Path $userDir 'libraries\TFT_eSPI\User_Setup.h'
    if (-not (Test-Path $setup)) { Die "TFT_eSPI is installed but User_Setup.h was not found at $setup" }

    $bak = "$setup.bak"
    if (-not (Test-Path $bak)) { Copy-Item $setup $bak; Ok 'original saved as User_Setup.h.bak' }

    $content = @'
// Generated by deploy.ps1 - GloaMind eyes.
// If the image is offset, or the colours are wrong, see docs/faq.md before
// changing anything: the two usual fixes are ST7735_GREENTAB and 27MHz.
#define USER_SETUP_INFO "GloaMind-ST7735"

#define ST7735_DRIVER
#define TFT_WIDTH  128
#define TFT_HEIGHT 160
#define ST7735_REDTAB

#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST   8

#define LOAD_GLCD
#define USE_HSPI_PORT

#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000
#define TFT_RGB_ORDER TFT_RGB
'@
    [System.IO.File]::WriteAllText($setup, $content, (New-Object System.Text.UTF8Encoding($false)))
    Ok "written to $setup"
}

function Find-Board ($cli) {
    if ($Port -ne '') { return $Port }
    $raw = (& $cli board list --format json 2>$null) -join "`n"
    if (-not $raw) { return '' }
    try { $obj = $raw | ConvertFrom-Json } catch { return '' }
    if (-not $obj.detected_ports) { return '' }

    $serial = @()
    foreach ($p in $obj.detected_ports) {
        if (-not $p.port) { continue }
        $label = '' + $p.port.protocol_label
        $isUsb = $label -match 'USB'
        $isEsp = $false
        if ($p.matching_boards) {
            foreach ($b in $p.matching_boards) {
                if ('' + $b.fqbn -match '^esp32:') { $isEsp = $true }
            }
        }
        if ($isEsp -or $isUsb) { $serial += $p.port.address }
    }
    if ($serial.Count -ge 1) { return $serial[0] }
    return ''
}

function Do-Flash ($cli) {
    Step 6 'Looking for the board'
    $p = Find-Board $cli
    if ($p -eq '') {
        Warn 'No board found.'
        Say  '     Plug the board into a USB port with a DATA cable (not a charge-only one).'
        Say  '     If it still does not appear, hold BOOT, tap RESET, release BOOT, then run again.'
        Die  'Nothing to flash.'
    }
    Ok "board on $p"

    Step 7 'Compiling'
    & $cli compile --fqbn $Fqbn $Sketch
    if ($LASTEXITCODE -ne 0) { Die 'Compile failed. The first line of red text is the real error.' }

    Step 8 'Uploading'
    & $cli compile --fqbn $Fqbn --upload -p $p $Sketch
    if ($LASTEXITCODE -ne 0) {
        Warn 'Upload failed. Common causes, in order: another program is holding the port'
        Say  '     (close the serial monitor), the cable is charge-only, or the board is not in download mode.'
        Say  '     Hold BOOT, tap RESET, release BOOT, run flash again.'
        Die  'Upload failed.'
    }
    Ok 'flashed'
    Say ''
    Say '     The panel should light up within two seconds.'
    Say '     White screen for a moment is normal. White screen forever is not - see docs/faq.md.'
}

function Do-Doctor ($cli) {
    Step 1 'Tools'
    if ($cli) { Ok "arduino-cli: $cli" } else { Warn 'arduino-cli: not installed (run setup)' }

    if ($cli) {
        Step 2 'Board index'
        $dump = (& $cli config dump --format json 2>$null) -join "`n"
        if ($dump -match [regex]::Escape($IndexUrl)) { Ok 'ESP32 mirror registered' }
        else { Warn 'ESP32 mirror not registered (run setup)' }

        Step 3 'Core'
        $list = (& $cli core list 2>$null) -join "`n"
        if ($list -match 'esp32:esp32\s+(\S+)') {
            Ok ("esp32 core " + $Matches[1])
        } else {
            Warn 'esp32 core: not installed (run setup)'
        }

        Step 4 'Library'
        $libs = (& $cli lib list 2>$null) -join "`n"
        if ($libs -match 'TFT_eSPI') { Ok 'TFT_eSPI installed' } else { Warn 'TFT_eSPI: not installed (run setup)' }

        Step 5 'Panel configuration'
        $userDir = Get-UserDir $cli
        $setup = Join-Path $userDir 'libraries\TFT_eSPI\User_Setup.h'
        if (Test-Path $setup) {
            $text = Get-Content $setup -Raw
            if ($text -match 'ST7735_DRIVER' -and $text -match 'TFT_MOSI 11') { Ok 'matches this build' }
            else { Warn 'installed, but not configured for this board (run setup)' }
        } else {
            Warn 'User_Setup.h not found'
        }

        Step 6 'Board'
        $p = Find-Board $cli
        if ($p -ne '') { Ok "connected on $p" }
        else {
            Warn 'no board detected'
            Say  '     checklist: data cable / driver installed / hold BOOT + tap RESET'
        }
    }

    Step 7 'Sketch'
    if (Test-Path (Join-Path $Sketch 'pet_eye.ino')) { Ok 'firmware\pet_eye found' }
    else { Die "firmware\pet_eye is missing. Run this from the repository folder." }

    Say ''
    Say '     doctor only looks. To fix what it reports, run:  deploy.ps1 setup'
}

# ---------------------------------------------------------------- entry point

Write-Host ''
Write-Host '  GloaMind eyes - deploy'
Write-Host "  mode: $Mode"
Write-Host ('  ' + ('-' * 46))

if ($Mode -eq 'doctor') {
    Do-Doctor (Find-Cli)
    exit 0
}

if ($Mode -eq 'setup' -or $Mode -eq 'all') {
    $cli = Install-Cli
    Ensure-Index $cli
    Ensure-Core $cli
    Ensure-Lib $cli
    Ensure-Setup $cli
    Say ''
    Ok 'setup finished'
    if ($Mode -eq 'setup') {
        Say '     Next: plug the board in and run  deploy.ps1 flash'
        exit 0
    }
}

if ($Mode -eq 'flash' -or $Mode -eq 'all') {
    $cli = Find-Cli
    if (-not $cli) { Die 'arduino-cli is not installed. Run:  deploy.ps1 setup' }
    Do-Flash $cli
}
