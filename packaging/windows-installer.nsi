Unicode true

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!ifndef PRODUCT_VERSION
  !error "Pass /DPRODUCT_VERSION=x.y.z"
!endif
!ifndef PAYLOAD_DIR
  !error "Pass /DPAYLOAD_DIR=absolute-path"
!endif
!ifndef OUTPUT_FILE
  !error "Pass /DOUTPUT_FILE=absolute-path"
!endif
!ifndef PRODUCT_ICON
  !error "Pass /DPRODUCT_ICON=absolute-path"
!endif

!ifndef PRODUCT_WELCOME_BMP
  !error "Pass /DPRODUCT_WELCOME_BMP=absolute-path"
!endif
!ifndef PRODUCT_HEADER_BMP
  !error "Pass /DPRODUCT_HEADER_BMP=absolute-path"
!endif

!define MUI_ICON "${PRODUCT_ICON}"
!define MUI_UNICON "${PRODUCT_ICON}"
!define MUI_WELCOMEFINISHPAGE_BITMAP "${PRODUCT_WELCOME_BMP}"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "${PRODUCT_WELCOME_BMP}"
!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_RIGHT
!define MUI_HEADERIMAGE_BITMAP "${PRODUCT_HEADER_BMP}"
!define MUI_HEADERIMAGE_UNBITMAP "${PRODUCT_HEADER_BMP}"
!define MUI_HEADERIMAGE_BITMAP_STRETCH "AspectFitHeight"
!define MUI_HEADERIMAGE_UNBITMAP_STRETCH "AspectFitHeight"
!define MUI_BGCOLOR "F7F9FF"


!define PRODUCT_NAME "视频压缩工作台"
!define PRODUCT_EXE "video-compression-workbench.exe"
!define PRODUCT_REGISTRY "Software\VideoCompressionWorkbench"
!define UNINSTALL_REGISTRY "Software\Microsoft\Windows\CurrentVersion\Uninstall\VideoCompressionWorkbench"

Name "${PRODUCT_NAME}"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\VideoCompressionWorkbench"
InstallDirRegKey HKCU "${PRODUCT_REGISTRY}" "InstallDir"
RequestExecutionLevel user
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show
BrandingText "Kenvix <i@kenvix.com>"

VIProductVersion "${PRODUCT_VERSION}.0"
VIAddVersionKey /LANG=0 "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey /LANG=0 "FileDescription" "视频压缩工作台安装程序"
VIAddVersionKey /LANG=0 "FileVersion" "${PRODUCT_VERSION}.0"
VIAddVersionKey /LANG=0 "ProductVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=0 "CompanyName" "Kenvix"
VIAddVersionKey /LANG=0 "LegalCopyright" "Copyright (C) 2026 Kenvix <i@kenvix.com>"
VIAddVersionKey /LANG=0 "Comments" "Author: Kenvix <i@kenvix.com>"
VIAddVersionKey /LANG=0 "OriginalFilename" "video-compression-workbench-setup.exe"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
!insertmacro MUI_LANGUAGE "English"

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_ICONSTOP "视频压缩工作台需要 64 位 Windows。"
        Abort
    ${EndIf}
FunctionEnd

Section "视频压缩工作台及 FFmpeg" SecMain
    SectionIn RO
    SetShellVarContext current
    SetRegView 64
    SetOutPath "$INSTDIR"
    File "${PAYLOAD_DIR}\video-compression-workbench.exe"
    File "${PAYLOAD_DIR}\README.md"

    SetOutPath "$INSTDIR\bin"
    File "${PAYLOAD_DIR}\bin\ffmpeg.exe"
    File "${PAYLOAD_DIR}\bin\ffprobe.exe"

    SetOutPath "$INSTDIR\docs"
    File /r "${PAYLOAD_DIR}\docs\*.*"

    SetOutPath "$INSTDIR"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    CreateDirectory "$SMPROGRAMS\视频压缩工作台"
    CreateShortCut "$SMPROGRAMS\视频压缩工作台\视频压缩工作台.lnk" "$INSTDIR\${PRODUCT_EXE}"
    CreateShortCut "$SMPROGRAMS\视频压缩工作台\卸载视频压缩工作台.lnk" "$INSTDIR\Uninstall.exe"

    WriteRegStr HKCU "${PRODUCT_REGISTRY}" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_REGISTRY}" "DisplayName" "${PRODUCT_NAME}"
    WriteRegStr HKCU "${UNINSTALL_REGISTRY}" "DisplayVersion" "${PRODUCT_VERSION}"
    WriteRegStr HKCU "${UNINSTALL_REGISTRY}" "Publisher" "Kenvix"
    WriteRegStr HKCU "${UNINSTALL_REGISTRY}" "Contact" "i@kenvix.com"
    WriteRegStr HKCU "${UNINSTALL_REGISTRY}" "URLInfoAbout" "https://github.com/HSOS6Miyabi/FFMPEG-UI"
    WriteRegStr HKCU "${UNINSTALL_REGISTRY}" "DisplayIcon" "$INSTDIR\${PRODUCT_EXE}"
    WriteRegStr HKCU "${UNINSTALL_REGISTRY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_REGISTRY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "${UNINSTALL_REGISTRY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegDWORD HKCU "${UNINSTALL_REGISTRY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINSTALL_REGISTRY}" "NoRepair" 1
SectionEnd

Section /o "创建桌面快捷方式" SecDesktop
    SetShellVarContext current
    CreateShortCut "$DESKTOP\视频压缩工作台.lnk" "$INSTDIR\${PRODUCT_EXE}"
SectionEnd

Section "Uninstall"
    SetShellVarContext current
    SetRegView 64
    Delete "$DESKTOP\视频压缩工作台.lnk"
    Delete "$SMPROGRAMS\视频压缩工作台\视频压缩工作台.lnk"
    Delete "$SMPROGRAMS\视频压缩工作台\卸载视频压缩工作台.lnk"
    RMDir "$SMPROGRAMS\视频压缩工作台"

    Delete "$INSTDIR\${PRODUCT_EXE}"
    Delete "$INSTDIR\README.md"
    Delete "$INSTDIR\bin\ffmpeg.exe"
    Delete "$INSTDIR\bin\ffprobe.exe"
    RMDir "$INSTDIR\bin"
    RMDir /r "$INSTDIR\docs"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"

    DeleteRegKey HKCU "${UNINSTALL_REGISTRY}"
    DeleteRegKey HKCU "${PRODUCT_REGISTRY}"
SectionEnd
