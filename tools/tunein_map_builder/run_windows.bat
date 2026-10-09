@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
set "TOOL_VENV=%LOCALAPPDATA%\FusionEdgeTuneInMapBuilder\venv"
set "TOOL_PYTHON=%TOOL_VENV%\Scripts\python.exe"

if not exist "%TOOL_PYTHON%" (
  echo Preparing the image converter for the first run...
  where py >nul 2>nul
  if not errorlevel 1 py -3 -m venv "%TOOL_VENV%"
)
if not exist "%TOOL_PYTHON%" (
  where python >nul 2>nul
  if not errorlevel 1 python -m venv "%TOOL_VENV%"
)
if not exist "%TOOL_PYTHON%" (
  if exist "%USERPROFILE%\.platformio\penv\Scripts\python.exe" (
    "%USERPROFILE%\.platformio\penv\Scripts\python.exe" -m venv "%TOOL_VENV%"
  )
)
if not exist "%TOOL_PYTHON%" (
  echo Python 3 with venv support was not found.
  pause
  exit /b 1
)

"%TOOL_PYTHON%" -c "from PIL import Image; import truststore" >nul 2>nul
if errorlevel 1 (
  echo Installing Pillow and truststore into the map builder's own environment...
  "%TOOL_PYTHON%" -m pip install --only-binary=:all: -r "%SCRIPT_DIR%requirements.txt"
  if errorlevel 1 (
    echo Dependency installation failed. Check the network connection, then try again.
    pause
    exit /b 1
  )
)

"%TOOL_PYTHON%" "%SCRIPT_DIR%tunein_map_builder.py" --gui
set "APP_EXIT=%errorlevel%"
if not "%APP_EXIT%"=="0" pause
exit /b %APP_EXIT%
