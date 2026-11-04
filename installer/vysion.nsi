; Vysion installer (NSIS). Per-user install, no admin prompt.
; Build:  makensis -DVERSION=0.2.1 installer/vysion.nsi   (expects build/Vysion.exe, build/mpv.dll, build/ffmpeg.exe)

Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"

!ifndef VERSION
  !define VERSION "0.2.1"
!endif
!define APP        "Vysion"
!define PUBLISHER  "Lauro Seibert"
!define URL        "https://github.com/LauroSeibert/Vysion"
!define UNINSTKEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\Vysion"
!define PROGID     "Vysion.Video"
!define CAPS       "Software\Vysion\Capabilities"

Name "${APP}"
OutFile "..\build\Vysion-Setup-${VERSION}.exe"
InstallDir "$LOCALAPPDATA\Programs\Vysion"
InstallDirRegKey HKCU "${UNINSTKEY}" "InstallLocation"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "${APP} ${VERSION}"
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "FileDescription" "${APP} Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "LegalCopyright" "${PUBLISHER}"

!define MUI_ICON "..\res\app.ico"
!define MUI_UNICON "..\res\app.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\Vysion.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start Vysion"
!define MUI_FINISHPAGE_SHOWREADME ""
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Choose Vysion as my default video player"
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION OpenDefaultApps
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; ---- close a running copy first (files would be locked) ----
!macro CloseRunning
  FindWindow $0 "VysionWindow"
  ${If} $0 <> 0
    MessageBox MB_OKCANCEL|MB_ICONINFORMATION "Vysion is running. Click OK to close it and continue." IDOK +2
    Abort
    SendMessage $0 ${WM_CLOSE} 0 0
    Sleep 1500
  ${EndIf}
!macroend

Function .onInit
  !insertmacro CloseRunning
FunctionEnd

; ---- one video extension: Open with list + Default apps capability ----
!macro Ext EXT
  WriteRegStr HKCU "Software\Classes\.${EXT}\OpenWithProgids" "${PROGID}" ""
  WriteRegStr HKCU "${CAPS}\FileAssociations" ".${EXT}" "${PROGID}"
  WriteRegStr HKCU "Software\Classes\Applications\Vysion.exe\SupportedTypes" ".${EXT}" ""
!macroend
!macro UnExt EXT
  DeleteRegValue HKCU "Software\Classes\.${EXT}\OpenWithProgids" "${PROGID}"
!macroend
!macro AllExt M
  !insertmacro ${M} "mp4"
  !insertmacro ${M} "m4v"
  !insertmacro ${M} "mkv"
  !insertmacro ${M} "webm"
  !insertmacro ${M} "avi"
  !insertmacro ${M} "mov"
  !insertmacro ${M} "wmv"
  !insertmacro ${M} "flv"
  !insertmacro ${M} "f4v"
  !insertmacro ${M} "mpg"
  !insertmacro ${M} "mpeg"
  !insertmacro ${M} "ts"
  !insertmacro ${M} "m2ts"
  !insertmacro ${M} "mts"
  !insertmacro ${M} "3gp"
  !insertmacro ${M} "ogv"
  !insertmacro ${M} "vob"
  !insertmacro ${M} "asf"
  !insertmacro ${M} "divx"
  !insertmacro ${M} "rmvb"
  !insertmacro ${M} "mxf"
!macroend

Section "Vysion (required)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File "..\build\Vysion.exe"
  File "..\build\mpv.dll"
  File "..\build\ffmpeg.exe"
  File "..\README.md"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; Start menu
  CreateShortcut "$SMPROGRAMS\Vysion.lnk" "$INSTDIR\Vysion.exe"

  ; Settings > Apps / Control Panel entry
  WriteRegStr   HKCU "${UNINSTKEY}" "DisplayName" "${APP}"
  WriteRegStr   HKCU "${UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr   HKCU "${UNINSTKEY}" "DisplayIcon" "$INSTDIR\Vysion.exe,0"
  WriteRegStr   HKCU "${UNINSTKEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr   HKCU "${UNINSTKEY}" "URLInfoAbout" "${URL}"
  WriteRegStr   HKCU "${UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr   HKCU "${UNINSTKEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr   HKCU "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoRepair" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "EstimatedSize" 70000

  ; file type (ProgID): plain video-file icon, no app badge on thumbnails
  WriteRegStr HKCU "Software\Classes\${PROGID}" "" "Video"
  WriteRegStr HKCU "Software\Classes\${PROGID}" "FriendlyTypeName" "Video"
  WriteRegStr HKCU "Software\Classes\${PROGID}" "TypeOverlay" ""
  WriteRegStr HKCU "Software\Classes\${PROGID}\DefaultIcon" "" '"$INSTDIR\Vysion.exe",1'
  WriteRegStr HKCU "Software\Classes\${PROGID}\shell\open" "FriendlyAppName" "${APP}"
  WriteRegStr HKCU "Software\Classes\${PROGID}\shell\open\command" "" '"$INSTDIR\Vysion.exe" "%1"'

  ; the application itself
  WriteRegStr HKCU "Software\Classes\Applications\Vysion.exe" "FriendlyAppName" "${APP}"
  WriteRegStr HKCU "Software\Classes\Applications\Vysion.exe" "TypeOverlay" ""
  WriteRegStr HKCU "Software\Classes\Applications\Vysion.exe\DefaultIcon" "" '"$INSTDIR\Vysion.exe",1'
  WriteRegStr HKCU "Software\Classes\Applications\Vysion.exe\shell\open\command" "" '"$INSTDIR\Vysion.exe" "%1"'

  ; Settings > Default apps
  WriteRegStr HKCU "${CAPS}" "ApplicationName" "${APP}"
  WriteRegStr HKCU "${CAPS}" "ApplicationDescription" "A tiny, instant-start video player with timeline previews."
  WriteRegStr HKCU "${CAPS}" "ApplicationIcon" "$INSTDIR\Vysion.exe,0"
  WriteRegStr HKCU "Software\RegisteredApplications" "${APP}" "${CAPS}"
  !insertmacro AllExt Ext

  ; tell Explorer associations changed
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

Section /o "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\Vysion.lnk" "$INSTDIR\Vysion.exe"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "The Vysion player."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "Put a Vysion shortcut on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Function OpenDefaultApps
  ExecShell "open" "ms-settings:defaultapps?registeredAppUser=Vysion"
FunctionEnd

; ---- uninstall ----
Function un.onInit
  FindWindow $0 "VysionWindow"
  ${If} $0 <> 0
    SendMessage $0 ${WM_CLOSE} 0 0
    Sleep 1500
  ${EndIf}
FunctionEnd

Section "Uninstall"
  Delete "$INSTDIR\Vysion.exe"
  Delete "$INSTDIR\mpv.dll"
  Delete "$INSTDIR\ffmpeg.exe"
  Delete "$INSTDIR\libmpv-2.dll"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  Delete "$SMPROGRAMS\Vysion.lnk"
  Delete "$DESKTOP\Vysion.lnk"

  !insertmacro AllExt UnExt
  DeleteRegKey HKCU "Software\Classes\${PROGID}"
  DeleteRegKey HKCU "Software\Classes\Applications\Vysion.exe"
  DeleteRegValue HKCU "Software\RegisteredApplications" "${APP}"
  DeleteRegKey HKCU "Software\Vysion"
  DeleteRegKey HKCU "${UNINSTKEY}"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

  ${IfNot} ${Silent}
    MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "Also delete your Vysion settings (volume, window size, repeat mode)?" IDNO +2
    RMDir /r "$APPDATA\Vysion"
  ${EndIf}
SectionEnd
