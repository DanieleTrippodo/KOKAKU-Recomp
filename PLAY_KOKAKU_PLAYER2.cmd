@echo off
setlocal
cd /d "%~dp0"
if not exist "tools\PSPRecomp\build\bin\KokakuNative.exe" (
    echo KokakuNative.exe non trovato. Compila prima il progetto.
    pause
    exit /b 1
)
echo Avvio Giocatore 2 (Batou - PortOffset 1000, MAC 02:00:00:00:00:02)...
"tools\PSPRecomp\build\bin\KokakuNative.exe" "extracted\PSP_GAME\USRDIR\main.psp.prx" "extracted" --port-offset 1000 --mac 02:00:00:00:00:02 --net-nickname Batou
set "kokaku_exit=%errorlevel%"
if not "%kokaku_exit%"=="0" if not "%kokaku_exit%"=="4" (
    echo Avvio terminato con errore %kokaku_exit%.
    pause
)
exit /b %kokaku_exit%
