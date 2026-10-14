@echo off
rem Tekken 3 Expanded for Android, on Windows: your game files once, then the
rem APK (Tekken3Expanded.apk) to install on an arm64 phone. Double-click it.
rem Needs Python 3.10 to 3.15 from python.org (with the py launcher and Tk):
rem tools\requirements-import.txt pins Pillow and numpy wheels for those only.
rem With options (--install, -D...) it runs tools\android\make_apk.py in the
rem console, for a game already prepared.
cd /d "%~dp0"
set "supported=import sys; sys.exit(not (3, 10) <= sys.version_info[:2] <= (3, 15))"
rem A venv made with another Python (newer, or one since uninstalled) is made again.
if exist ".setup\venv\Scripts\python.exe" (
    .setup\venv\Scripts\python.exe -c "%supported%" >nul 2>&1 || rmdir /s /q .setup\venv
)
if exist ".setup\venv\Scripts\python.exe" goto venv_ready
rem <nul: a py that offers to install a missing version must not wait unseen.
set "python="
for %%v in (3.13 3.12 3.14 3.11 3.10 3.15 3) do if not defined python (py -%%v -c "%supported%" <nul >nul 2>&1 && set "python=-%%v")
if not defined python (
    echo Tekken 3 Expanded needs Python 3.10 to 3.15 ^(3.13 recommended^); newer versions are not supported yet.
    echo Install Python 3.13 from https://www.python.org/downloads/ ^(beside your other Python if you like^),
    echo with "py launcher" and "tcl/tk and IDLE" ticked, then run this again.
    pause & exit /b 1
)
py %python% -m venv .setup\venv || (echo Could not create .setup\venv with py %python%.& pause & exit /b 1)
:venv_ready
rem Also after an install cut short (no network, window closed): the venv then exists without them.
.setup\venv\Scripts\python.exe -c "import PIL, numpy" 2>nul || (
    .setup\venv\Scripts\python.exe -m pip install --quiet --require-hashes --only-binary=:all: -r tools\requirements-pip.txt || (echo Could not install pip.& pause & exit /b 1)
    .setup\venv\Scripts\python.exe -m pip install --quiet --require-hashes --only-binary=:all: -r tools\requirements-import.txt || (echo Could not install Pillow and numpy.& pause & exit /b 1)
)
if "%~1"=="" (
    start "" ".setup\venv\Scripts\pythonw.exe" launcher\easy_launcher.py --android-only
    exit /b 0
)
".setup\venv\Scripts\python.exe" tools\android\make_apk.py %*
set status=%errorlevel%
pause
exit /b %status%
