@echo off
chcp 65001 >nul
echo ==============================================
echo 小智固件一键烧录脚本 (基于 esptool)
echo Target Project: ZhiYin-HBG-V1.0.2
echo Target Chip: esp32s3
echo ==============================================

cd /d "%~dp0"

set ESPTOOL_PY=C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe -m esptool
set BIN_FILE=build\xiaozhi_merged_full.bin

if not exist "%BIN_FILE%" (
    echo [错误] 找不到合并后的固件文件: %BIN_FILE%
    echo 请确认是否已经成功编译并合并了固件。
    pause
    exit /b 1
)

set /p COMPORT="请输入设备所在的 COM 端口号 (例如 COM3, 直接回车将自动检测): "

set PORT_CMD=
if not "%COMPORT%"=="" set PORT_CMD=--port %COMPORT%

echo.
echo 准备开始烧录，请保持设备连接，期间请勿断开 USB 线...
echo 烧录命令: %ESPTOOL_PY% --chip esp32s3 %PORT_CMD% -b 1152000 write_flash 0x0 %BIN_FILE%
echo.

%ESPTOOL_PY% --chip esp32s3 %PORT_CMD% -b 1152000 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x0 "%BIN_FILE%"

echo.
echo ==============================================
echo 烧录操作结束。如果提示 "Hash of data verified"，说明烧录成功！
echo 设备应该已自动重启。
echo ==============================================
pause
