@echo off
chcp 936 >nul
title AURIX Flash - E8_08_mt9v03x_uart_seekfree_assistant_demo
cd /d "%~dp0Debug"

echo ==================================================
echo   AURIX 一键烧写  E8_08_mt9v03x_uart_seekfree_assistant_demo
echo ==================================================
echo.

if not exist "E8_08_mt9v03x_uart_seekfree_assistant_demo.hex" (
    echo [错误] 找不到 E8_08_mt9v03x_uart_seekfree_assistant_demo.hex
    echo 请先在 ADS 里点锤子编译工程，再运行本脚本。
    echo.
    pause
    exit /b 1
)

echo 待烧写文件及编译时间：
for %%F in ("E8_08_mt9v03x_uart_seekfree_assistant_demo.hex") do echo   %%~nxF    %%~tF
echo.

echo 清理残留的烧写进程...
taskkill /F /IM AURIXFlasher.exe >nul 2>&1

echo 开始烧写...
echo.
"F:\ProjectFor\ADS\AURIX-Studio-1.10.10\tools\AurixFlasherSoftwareTool_v3.0.4\AURIXFlasher.exe" -hex E8_08_mt9v03x_uart_seekfree_assistant_demo.hex
echo.
echo --------------------------------------------------
echo 看到 "AURIXFlasher Exit Status: Pass" 即为成功。
echo --------------------------------------------------
echo.
pause
