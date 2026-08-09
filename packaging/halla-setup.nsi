; ============================================================================
; Halla — Instalador NSIS (Windows 64-bit)
; Produz Halla-Setup-3.13.0.exe, um instalador clássico no estilo dos
; aplicativos Windows: licença, pasta de destino, atalhos e desinstalador.
; ============================================================================

Unicode true
!include "MUI2.nsh"

!define APP_NAME      "Halla"
!define APP_VERSION   "1.0.37"
!define APP_PUBLISHER "Halla"
!define APP_EXE       "Halla.exe"
!define APP_DIR_REGKEY "Software\Microsoft\Windows\CurrentVersion\App Paths\${APP_EXE}"
!define UNINSTALL_REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"

Name "${APP_NAME} ${APP_VERSION}"
OutFile "..\Halla-Setup-${APP_VERSION}.exe"
InstallDir "$PROGRAMFILES64\${APP_NAME}"
InstallDirRegKey HKLM "${APP_DIR_REGKEY}" ""
RequestExecutionLevel admin
SetCompressor /SOLID lzma
ManifestDPIAware true

; ícones do instalador / desinstalador
!define MUI_ICON   "..\src\halla.ico"
!define MUI_UNICON "..\src\halla.ico"
!define MUI_WELCOMEFINISHPAGE_BITMAP "..\src\installer-side.bmp"
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Executar o ${APP_NAME} agora"

; páginas do instalador
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

; páginas do desinstalador
!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "PortugueseBR"

; ----------------------------------------------------------------------------
Section "Halla (obrigatório)" SEC01
    SetOutPath "$INSTDIR"

    File /r "..\dist\Halla\*"

    ; guarda o caminho de instalação
    WriteRegStr HKLM "${APP_DIR_REGKEY}" "" "$INSTDIR\${APP_EXE}"
    WriteRegStr HKLM "${APP_DIR_REGKEY}" "Path" "$INSTDIR"

    ; desinstalador
    WriteUninstaller "$INSTDIR\Desinstalar.exe"

    ; entrada "Adicionar ou remover programas"
    WriteRegStr HKLM "${UNINSTALL_REGKEY}" "DisplayName"     "${APP_NAME}"
    WriteRegStr HKLM "${UNINSTALL_REGKEY}" "DisplayVersion"  "${APP_VERSION}"
    WriteRegStr HKLM "${UNINSTALL_REGKEY}" "Publisher"       "${APP_PUBLISHER}"
    WriteRegStr HKLM "${UNINSTALL_REGKEY}" "DisplayIcon"     "$INSTDIR\${APP_EXE}"
    WriteRegStr HKLM "${UNINSTALL_REGKEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${UNINSTALL_REGKEY}" "UninstallString" "$INSTDIR\Desinstalar.exe"
    WriteRegDWORD HKLM "${UNINSTALL_REGKEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINSTALL_REGKEY}" "NoRepair" 1

    ; atalhos: Menu Iniciar + Área de trabalho
    CreateDirectory "$SMPROGRAMS\${APP_NAME}"
    CreateShortcut  "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
    CreateShortcut  "$SMPROGRAMS\${APP_NAME}\Desinstalar ${APP_NAME}.lnk" "$INSTDIR\Desinstalar.exe"
    CreateShortcut  "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
SectionEnd

; ----------------------------------------------------------------------------
Section "Uninstall"
    ; remove arquivos do programa
    Delete "$INSTDIR\${APP_EXE}"
    Delete "$INSTDIR\Qt6Core.dll"
    Delete "$INSTDIR\Qt6Gui.dll"
    Delete "$INSTDIR\Qt6Widgets.dll"
    Delete "$INSTDIR\Qt6Network.dll"
    Delete "$INSTDIR\Qt6Multimedia.dll"
    Delete "$INSTDIR\Qt6TextToSpeech.dll"
    Delete "$INSTDIR\libgcc_s_seh-1.dll"
    Delete "$INSTDIR\libstdc++-6.dll"
    Delete "$INSTDIR\libwinpthread-1.dll"
    Delete "$INSTDIR\LEIA-ME.txt"
    Delete "$INSTDIR\platforms\qwindows.dll"
    Delete "$INSTDIR\styles\qmodernwindowsstyle.dll"
    Delete "$INSTDIR\multimedia\windowsmediaplugin.dll"
    Delete "$INSTDIR\tls\qschannelbackend.dll"
    Delete "$INSTDIR\texttospeech\qtexttospeech_sapi.dll"
    Delete "$INSTDIR\imageformats\qgif.dll"
    Delete "$INSTDIR\imageformats\qjpeg.dll"
    Delete "$INSTDIR\imageformats\qico.dll"
    Delete "$INSTDIR\Desinstalar.exe"
    RMDir  "$INSTDIR\platforms"
    RMDir  "$INSTDIR\styles"
    RMDir  "$INSTDIR\multimedia"
    RMDir  "$INSTDIR\tls"
    RMDir  "$INSTDIR\texttospeech"
    RMDir  "$INSTDIR\imageformats"
    RMDir  "$INSTDIR"

    ; atalhos
    Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
    Delete "$SMPROGRAMS\${APP_NAME}\Desinstalar ${APP_NAME}.lnk"
    RMDir  "$SMPROGRAMS\${APP_NAME}"
    Delete "$DESKTOP\${APP_NAME}.lnk"

    ; registro
    DeleteRegKey HKLM "${UNINSTALL_REGKEY}"
    DeleteRegKey HKLM "${APP_DIR_REGKEY}"
SectionEnd
