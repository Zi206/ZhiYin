@echo off
echo ==============================================
echo Activating ESP-IDF v5.5.4 Environment
echo Target Project: ZhiYin-HBG-V1.0.2
echo Target Chip: esp32s3
echo ==============================================

set IDF_PATH=A:\esp-idf\.espressif\v5.5.4\esp-idf
set IDF_TOOLS_PATH=C:\Espressif\tools
set IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v5.5.4\venv

set PATH=C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;%PATH%

cd /d "%~dp0"

echo Keeping build folder for incremental build...
:: if exist build rmdir /s /q build

echo [1/2] Skipping set target to preserve sdkconfig...
:: C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe A:\esp-idf\.espressif\v5.5.4\esp-idf\tools\idf.py set-target esp32s3

echo [2/2] Starting compilation...
C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe A:\esp-idf\.espressif\v5.5.4\esp-idf\tools\idf.py build

echo ==============================================
echo Build Completed.
pause
