; Halla Desktop — instalador NSIS Windows x64
Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"

!ifndef APP_VERSION
  !error "APP_VERSION é obrigatório. Use makensis /DAPP_VERSION=<VERSION>."
!endif

!define APP_NAME       "Halla"
!define APP_DISPLAY    "Halla Desktop"
!define APP_PUBLISHER  "Halla-DEV"
!define APP_EXE        "Halla.exe"
!define APP_DIR_REGKEY "Software\Microsoft\Windows\CurrentVersion\App Paths\${APP_EXE}"
!define UNINSTALL_KEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"

Name "${APP_DISPLAY} ${APP_VERSION}"
Caption "Instalação do ${APP_DISPLAY} ${APP_VERSION}"
BrandingText "Halla-DEV"
OutFile "..\Halla-Setup-${APP_VERSION}.exe"
InstallDir "$PROGRAMFILES64\${APP_NAME}"
InstallDirRegKey HKLM "${APP_DIR_REGKEY}" ""
RequestExecutionLevel admin
SetCompressor /SOLID lzma
ManifestDPIAware true
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey /LANG=1046 "CompanyName" "${APP_PUBLISHER}"
VIAddVersionKey /LANG=1046 "FileDescription" "Instalador do ${APP_DISPLAY}"
VIAddVersionKey /LANG=1046 "FileVersion" "${APP_VERSION}.0"
VIAddVersionKey /LANG=1046 "InternalName" "Halla-Setup"
VIAddVersionKey /LANG=1046 "LegalCopyright" "Copyright 2026 ${APP_PUBLISHER}"
VIAddVersionKey /LANG=1046 "OriginalFilename" "Halla-Setup-${APP_VERSION}.exe"
VIAddVersionKey /LANG=1046 "ProductName" "${APP_DISPLAY}"
VIAddVersionKey /LANG=1046 "ProductVersion" "${APP_VERSION}"

!define MUI_ABORTWARNING
!define MUI_ICON "..\src\halla.ico"
!define MUI_UNICON "..\src\halla.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "..\src\installer-side.bmp"
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Executar o ${APP_NAME} agora"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "PortugueseBR"

Section "Halla (obrigatório)" SEC_HALLA
    SectionIn RO
    SetShellVarContext all
    SetRegView 64
    SetOutPath "$INSTDIR"
    File /r "..\dist\Halla\*"

    ; Builds MSVC incluem o redistribuível. Builds MinGW simplesmente pulam esta etapa.
    IfFileExists "$INSTDIR\vc_redist.x64.exe" 0 runtime_done
    ExecWait '"$INSTDIR\vc_redist.x64.exe" /install /quiet /norestart' $0
    ${If} $0 == 3010
        SetRebootFlag true
    ${EndIf}
    Delete "$INSTDIR\vc_redist.x64.exe"
    runtime_done:

    WriteRegStr HKLM "${APP_DIR_REGKEY}" "" "$INSTDIR\${APP_EXE}"
    WriteRegStr HKLM "${APP_DIR_REGKEY}" "Path" "$INSTDIR"
    WriteUninstaller "$INSTDIR\Desinstalar.exe"

    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "${APP_DISPLAY}"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "${APP_PUBLISHER}"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Desinstalar.exe"'
    WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Desinstalar.exe" /S'
    WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1

    CreateDirectory "$SMPROGRAMS\${APP_NAME}"
    CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
    CreateShortcut "$SMPROGRAMS\${APP_NAME}\Desinstalar ${APP_NAME}.lnk" "$INSTDIR\Desinstalar.exe"
    CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
SectionEnd

Section "Uninstall"
    SetShellVarContext all
    SetRegView 64
    Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
    Delete "$SMPROGRAMS\${APP_NAME}\Desinstalar ${APP_NAME}.lnk"
    RMDir "$SMPROGRAMS\${APP_NAME}"
    Delete "$DESKTOP\${APP_NAME}.lnk"
    DeleteRegKey HKLM "${UNINSTALL_KEY}"
    DeleteRegKey HKLM "${APP_DIR_REGKEY}"
    RMDir /r "$INSTDIR"
SectionEnd
