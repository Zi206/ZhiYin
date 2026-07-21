@echo off
set IDF_PATH=A:\esp-idf\.espressif\v5.5.4\esp-idf
set IDF_TOOLS_PATH=C:\Espressif\tools
set IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v5.5.4\venv
set PATH=C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;%PATH%
cd /d "%~dp0"

echo ==============================================
echo [1/2] Checking Memory/RAM Usage...
C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe A:\esp-idf\.espressif\v5.5.4\esp-idf\tools\idf.py size

echo ==============================================
echo [2/2] Merging binaries into one full firmware...
C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe -m esptool --chip esp32s3 merge_bin -o build\xiaozhi_merged_full.bin --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0xd000 build\ota_data_initial.bin 0x20000 build\xiaozhi.bin 0x800000 build\generated_assets.bin

echo ==============================================
echo Merge complete! Output: build\xiaozhi_merged_full.bin
pause
