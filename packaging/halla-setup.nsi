; Halla Desktop — instalador NSIS Windows x64
; Multilíngue desde a v1.1.31: inglês, português (Brasil) e espanhol, com
; diálogo de seleção na abertura e a escolha memorizada no registro.
; IMPORTANTE: este arquivo é UTF-8 COM BOM — sem o BOM o makensis lê os
; acentos na página de código ANSI e o instalador fica com caracteres
; corrompidos ("InstalaÃ§Ã£o").
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
VIAddVersionKey /LANG=1033 "CompanyName" "${APP_PUBLISHER}"
VIAddVersionKey /LANG=1033 "FileDescription" "Halla Desktop installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${APP_VERSION}.0"
VIAddVersionKey /LANG=1033 "InternalName" "Halla-Setup"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright 2026 ${APP_PUBLISHER}"
VIAddVersionKey /LANG=1033 "OriginalFilename" "Halla-Setup-${APP_VERSION}.exe"
VIAddVersionKey /LANG=1033 "ProductName" "${APP_DISPLAY}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${APP_VERSION}"
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
!define MUI_FINISHPAGE_RUN_TEXT "$(RUN_TEXT)"

; O idioma escolhido no diálogo fica salvo junto das informações de
; desinstalação — atualizações e o desinstalador reutilizam a escolha.
!define MUI_LANGDLL_REGISTRY_ROOT "HKLM"
!define MUI_LANGDLL_REGISTRY_KEY "${UNINSTALL_KEY}"
!define MUI_LANGDLL_REGISTRY_VALUENAME "InstallerLanguage"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "LICENSE.txt"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_WELCOME
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

; Primeiro idioma = pré-selecionado quando o sistema não for EN/PT-BR/ES.
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "PortugueseBR"
!insertmacro MUI_LANGUAGE "Spanish"

LangString SEC_HALLA_NAME ${LANG_ENGLISH} "Halla (required)"
LangString SEC_HALLA_NAME ${LANG_PORTUGUESEBR} "Halla (obrigatório)"
LangString SEC_HALLA_NAME ${LANG_SPANISH} "Halla (obligatorio)"

LangString RUN_TEXT ${LANG_ENGLISH} "Run Halla now"
LangString RUN_TEXT ${LANG_PORTUGUESEBR} "Executar o Halla agora"
LangString RUN_TEXT ${LANG_SPANISH} "Ejecutar Halla ahora"

LangString ADDON_DESC ${LANG_ENGLISH} "Halla add-on package"
LangString ADDON_DESC ${LANG_PORTUGUESEBR} "Pacote de complemento do Halla"
LangString ADDON_DESC ${LANG_SPANISH} "Paquete de complemento de Halla"

LangString UNINST_SHORTCUT ${LANG_ENGLISH} "Uninstall Halla"
LangString UNINST_SHORTCUT ${LANG_PORTUGUESEBR} "Desinstalar Halla"
LangString UNINST_SHORTCUT ${LANG_SPANISH} "Desinstalar Halla"

Function .onInit
    SetRegView 64
    ; Diálogo de idioma (inglês, português, espanhol) — suprimido em /S.
    ${IfNot} ${Silent}
        !insertmacro MUI_LANGDLL_DISPLAY
    ${EndIf}
FunctionEnd

Function un.onInit
    SetRegView 64
    !insertmacro MUI_UNGETLANGUAGE
FunctionEnd

Section "$(SEC_HALLA_NAME)" SEC_HALLA
    SectionIn RO
    SetShellVarContext all
    SetRegView 64
    SetOutPath "$INSTDIR"
    File /r "..\dist\Halla\*"
    ; SDK pequeno para autores de plugins da comunidade.
    SetOutPath "$INSTDIR\Plugin SDK"
    File "..\sdk\halla_plugin_api.h"
    File "..\sdk\LICENSE.txt"
    File "..\docs\PLUGINS.md"
    SetOutPath "$INSTDIR\Plugin SDK\hello_world"
    File "..\examples\plugins\hello_world\hello_world.cpp"
    File "..\examples\plugins\hello_world\CMakeLists.txt"
    File "..\examples\plugins\hello_world\manifest.json"
    SetOutPath "$INSTDIR\Plugin SDK\advanced_sdk"
    File "..\examples\plugins\advanced_sdk\advanced_sdk.cpp"
    File "..\examples\plugins\advanced_sdk\CMakeLists.txt"
    File "..\examples\plugins\advanced_sdk\manifest.json"
    SetOutPath "$INSTDIR"

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
    ; Pacotes comunitários: duplo clique abre a confirmação segura no Halla.
    WriteRegStr HKCR ".halla-addon" "" "HallaAddonPackage"
    WriteRegStr HKCR "HallaAddonPackage" "" "$(ADDON_DESC)"
    WriteRegStr HKCR "HallaAddonPackage\DefaultIcon" "" "$INSTDIR\${APP_EXE},0"
    WriteRegStr HKCR "HallaAddonPackage\shell\open\command" "" '$\"$INSTDIR\${APP_EXE}$\" $\"%1$\"'
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
    CreateShortcut "$SMPROGRAMS\${APP_NAME}\$(UNINST_SHORTCUT).lnk" "$INSTDIR\Desinstalar.exe"
    CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
SectionEnd

Section "Uninstall"
    SetShellVarContext all
    SetRegView 64
    Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
    Delete "$SMPROGRAMS\${APP_NAME}\$(UNINST_SHORTCUT).lnk"
    ; Atalho legado dos instaladores anteriores (sempre em português).
    Delete "$SMPROGRAMS\${APP_NAME}\Desinstalar Halla.lnk"
    RMDir "$SMPROGRAMS\${APP_NAME}"
    Delete "$DESKTOP\${APP_NAME}.lnk"
    DeleteRegKey HKLM "${UNINSTALL_KEY}"
    DeleteRegKey HKLM "${APP_DIR_REGKEY}"
    DeleteRegKey HKCR "HallaAddonPackage"
    DeleteRegKey HKCR ".halla-addon"
    RMDir /r "$INSTDIR"
SectionEnd
