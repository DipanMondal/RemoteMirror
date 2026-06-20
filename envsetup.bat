:: This bat file is used to activate the VisualStudio Environment for compilation and development
:: For normal installation, you can run this bat file as it is and start development locally
:: Otherwise, you can ignore this file.

@echo off

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    call "%%i\Common7\Tools\VsDevCmd.bat" -arch=x64
)

echo Visual Studio Developer Environment Loaded