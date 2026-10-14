@echo off
setlocal enabledelayedexpansion
title OpenWirelessMC v1.0 - Flasher
echo ========================================================
echo    OpenWirelessMC v1.0 - Firmware Flasher
echo ========================================================
echo.
set /p PORT="Digite a porta COM do ESP32 (exemplo: COM6): "
if "%PORT%"=="" (
    echo Porta invalida!
    pause
    exit /b 1
)

echo.
echo Gravando OpenWirelessMC (Bootloader + Particoes Dual-OTA + Firmware v1.0)...
python -m esptool --chip esp32 --port %PORT% --baud 921600 --before default_reset --after hard_reset write_flash -z --flash_mode dio --flash_freq 40m --flash_size 4MB 0x1000 bootloader.bin 0x8000 partitions.bin 0xd000 ota_data_initial.bin 0x10000 firmware_v1.0.bin

if %ERRORLEVEL% EQU 0 (
    echo.
    echo ========================================================
    echo    GRAVACAO CONCLUIDA COM SUCESSO!
    echo    O ESP32 ja possui suporte a Web OTA via Wi-Fi!
    echo ========================================================
) else (
    echo.
    echo [ERRO] Falha na gravacao.
)
pause
