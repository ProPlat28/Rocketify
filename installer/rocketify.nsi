; Rocketify - Windows setup wizard (NSIS / Modern UI 2)
; One installer for both the Steam and the Epic Games version of Rocket League.
; Build with ../build.sh  -> installer/Rocketify-Setup.exe

Unicode true
SetCompressor /SOLID lzma

!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "WordFunc.nsh"

!define APPNAME   "Rocketify"
!define VERSION   "0.0.2-alpha"
!define VERSION_NUM "0.0.2.0"   ; Windows needs four plain numbers
!define PUBLISHER "Rocketify (unofficial)"
!define REGKEY    "Software\Microsoft\Windows\CurrentVersion\Uninstall\Rocketify"

Name "${APPNAME}"
!ifndef BUILD
  !define BUILD "../build"
!endif
!ifndef OUTDIR
  !define OUTDIR "."
!endif
OutFile "${OUTDIR}/Rocketify-Setup.exe"
RequestExecutionLevel admin
BrandingText "Rocketify ${VERSION}"
InstallDir ""
ShowInstDetails show
ShowUninstDetails show

VIProductVersion "${VERSION_NUM}"
VIAddVersionKey "ProductName" "${APPNAME}"
VIAddVersionKey "FileDescription" "${APPNAME} Setup (Steam and Epic Games)"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "LegalCopyright" "Unofficial mod loader - not affiliated with Psyonix or Epic Games"

Var Win64Dir     ; $INSTDIR\Binaries\Win64
Var SteamDir     ; detected Steam install ("" if none)
Var EpicDir      ; detected Epic install ("" if none)
Var Edition      ; "Steam" or "Epic Games"
Var RadioSteam
Var RadioEpic
Var FinishLine     ; read-only box with the launch option
Var FinishCopy     ; Copy button
Var FinishRun      ; "Start Rocketify now" checkbox

; ---------------------------------------------------------------- pages
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "Welcome to ${APPNAME} Setup"
!define MUI_WELCOMEPAGE_TEXT "This wizard installs the mod loader into Rocket League - Steam or Epic Games version.$\r$\n$\r$\nYou get a MODS entry in the main menu and a mods folder for your own mods.$\r$\n$\r$\nPlease close Rocket League before continuing.$\r$\n$\r$\nClick Next to continue."
!insertmacro MUI_PAGE_WELCOME

!define MUI_LICENSEPAGE_TEXT_TOP "Please read the following before installing."
!define MUI_LICENSEPAGE_BUTTON "I &Understand"
!define MUI_LICENSEPAGE_TEXT_BOTTOM "Click I Understand to continue."
!insertmacro MUI_PAGE_LICENSE "readme.txt"

Page custom PlatformPage PlatformLeave

!define MUI_DIRECTORYPAGE_TEXT_TOP "The mods are installed into Rocket League's Binaries\Win64 folder (the one with RocketLeague.exe). Check the folder below.$\r$\n$\r$\nUse Browse if your game is somewhere else - picking the main Rocket League folder works too."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "Rocket League\Binaries\Win64 folder"
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE CheckGameFolder
!insertmacro MUI_PAGE_DIRECTORY

!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_TITLE "${APPNAME} is installed"
!define MUI_FINISHPAGE_TEXT " "
!define MUI_PAGE_CUSTOMFUNCTION_SHOW FinishShow
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE FinishLeave
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------- helpers
; Parses a  "key"  "value"  /  "key": "value"  line.
; In: $R1 = line. Out: $R2 = key, $R3 = value ("\\" unescaped to "\").
Function ParseKeyValue
  Push $0
  Push $1
  Push $2
  StrCpy $R2 ""
  StrCpy $R3 ""
  StrCpy $0 0
  StrCpy $1 0
  ${Do}
    StrCpy $2 $R1 1 $0
    ${If} $2 == ""
      ${Break}
    ${EndIf}
    ${If} $2 == '"'
      IntOp $1 $1 + 1
    ${ElseIf} $1 == 1
      StrCpy $R2 "$R2$2"
    ${ElseIf} $1 == 3
      StrCpy $R3 "$R3$2"
    ${EndIf}
    IntOp $0 $0 + 1
  ${Loop}
  ${WordReplace} "$R3" "\\" "\" "+" $R3
  Pop $2
  Pop $1
  Pop $0
FunctionEnd

; Steam: registry + every library folder listed in libraryfolders.vdf
Function DetectSteam
  StrCpy $SteamDir ""
  ReadRegStr $0 HKCU "Software\Valve\Steam" "SteamPath"
  ${If} $0 == ""
    Return
  ${EndIf}
  ${WordReplace} "$0" "/" "\" "+" $0
  ${If} ${FileExists} "$0\steamapps\common\rocketleague\Binaries\Win64\RocketLeague.exe"
    StrCpy $SteamDir "$0\steamapps\common\rocketleague"
  ${EndIf}
  ClearErrors
  FileOpen $4 "$0\steamapps\libraryfolders.vdf" r
  ${If} ${Errors}
    Return
  ${EndIf}
  ${Do}
    FileRead $4 $R1
    ${If} ${Errors}
      ${Break}
    ${EndIf}
    Call ParseKeyValue
    ${If} $R2 == "path"
    ${AndIf} $SteamDir == ""
    ${AndIf} ${FileExists} "$R3\steamapps\common\rocketleague\Binaries\Win64\RocketLeague.exe"
      StrCpy $SteamDir "$R3\steamapps\common\rocketleague"
    ${EndIf}
  ${Loop}
  FileClose $4
FunctionEnd

; Epic: InstallLocation in the launcher's manifest files
Function DetectEpic
  StrCpy $EpicDir ""
  SetShellVarContext all
  StrCpy $7 "$APPDATA\Epic\EpicGamesLauncher\Data\Manifests"
  SetShellVarContext current
  FindFirst $5 $6 "$7\*.item"
  ${DoWhile} $6 != ""
    ClearErrors
    FileOpen $4 "$7\$6" r
    ${IfNot} ${Errors}
      ${Do}
        FileRead $4 $R1
        ${If} ${Errors}
          ${Break}
        ${EndIf}
        Call ParseKeyValue
        ${If} $R2 == "InstallLocation"
        ${AndIf} $EpicDir == ""
        ${AndIf} ${FileExists} "$R3\Binaries\Win64\RocketLeague.exe"
          StrCpy $EpicDir $R3
        ${EndIf}
      ${Loop}
      FileClose $4
    ${EndIf}
    FindNext $5 $6
  ${Loop}
  FindClose $5
FunctionEnd

Function .onInit
  SetRegView 64
  Call DetectSteam
  Call DetectEpic
  ; preselect whichever version was found (Steam first)
  ${If} $SteamDir != ""
    StrCpy $Edition "Steam"
  ${ElseIf} $EpicDir != ""
    StrCpy $Edition "Epic Games"
  ${Else}
    StrCpy $Edition "Steam"
  ${EndIf}
FunctionEnd

; ---------------------------------------------------------------- platform page
Function PlatformPage
  !insertmacro MUI_HEADER_TEXT "Choose your version" "Which Rocket League do you play?"
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0 0 100% 24u "Select the version of Rocket League to install the mods into. Setup already looked for both."
  Pop $0

  ${If} $SteamDir != ""
    StrCpy $1 "Steam - found in $SteamDir"
  ${Else}
    StrCpy $1 "Steam - not found automatically (you can pick the folder next)"
  ${EndIf}
  ${NSD_CreateRadioButton} 10u 34u 100% 14u "$1"
  Pop $RadioSteam

  ${If} $EpicDir != ""
    StrCpy $1 "Epic Games - found in $EpicDir"
  ${Else}
    StrCpy $1 "Epic Games - not found automatically (you can pick the folder next)"
  ${EndIf}
  ${NSD_CreateRadioButton} 10u 54u 100% 14u "$1"
  Pop $RadioEpic

  ${If} $Edition == "Epic Games"
    ${NSD_Check} $RadioEpic
  ${Else}
    ${NSD_Check} $RadioSteam
  ${EndIf}
  nsDialogs::Show
FunctionEnd

Function PlatformLeave
  ${NSD_GetState} $RadioEpic $0
  ${If} $0 == ${BST_CHECKED}
    StrCpy $Edition "Epic Games"
    ${If} $EpicDir != ""
      StrCpy $INSTDIR "$EpicDir\Binaries\Win64"
    ${Else}
      StrCpy $INSTDIR "$PROGRAMFILES64\Epic Games\rocketleague\Binaries\Win64"
    ${EndIf}
  ${Else}
    StrCpy $Edition "Steam"
    ${If} $SteamDir != ""
      StrCpy $INSTDIR "$SteamDir\Binaries\Win64"
    ${Else}
      StrCpy $INSTDIR "$PROGRAMFILES32\Steam\steamapps\common\rocketleague\Binaries\Win64"
    ${EndIf}
  ${EndIf}
FunctionEnd

; Directory page: accept the game root or Binaries\Win64 itself.
Function NormalizeWin64Dir
  ; accept the game root and point at Binaries\Win64
  ${If} ${FileExists} "$INSTDIR\Binaries\Win64\RocketLeague.exe"
    StrCpy $INSTDIR "$INSTDIR\Binaries\Win64"
  ${EndIf}
  StrCpy $Win64Dir $INSTDIR
FunctionEnd

; Directory page: $INSTDIR must be Rocket League\Binaries\Win64.
Function CheckGameFolder
  Call NormalizeWin64Dir
  ${IfNot} ${FileExists} "$INSTDIR\RocketLeague.exe"
    MessageBox MB_ICONEXCLAMATION "This folder does not contain Rocket League.$\r$\n$\r$\nPick Rocket League's Binaries\Win64 folder (the one with RocketLeague.exe)."
    Abort
  ${EndIf}
FunctionEnd

; ---------------------------------------------------------------- finish page
Function FinishShow
  ; replace the stock text with our own layout
  ShowWindow $mui.FinishPage.Text ${SW_HIDE}

  ${If} $Edition == "Steam"
    ${NSD_CreateLabel} 120u 45u 195u 42u "Don't forget: add this to the Launch Options of Rocket League in Steam$\r$\n(Library > right-click Rocket League > Properties > General > Launch Options), then press Play:"
    Pop $0
    SetCtlColors $0 "" "ffffff"

    ${NSD_CreateText} 120u 89u 195u 13u '"$Win64Dir\Rocketify.exe" %command%'
    Pop $FinishLine
    SendMessage $FinishLine ${EM_SETREADONLY} 1 0

    ${NSD_CreateButton} 120u 106u 80u 15u "&Copy"
    Pop $FinishCopy
    ${NSD_OnClick} $FinishCopy OnCopyClick

    ${NSD_CreateLabel} 120u 127u 195u 30u "To play online with Easy Anti-Cheat, clear that Launch Options field again. You can also start the mods with the Rocketify shortcut."
    Pop $0
    SetCtlColors $0 "" "ffffff"
  ${Else}
    ${NSD_CreateLabel} 120u 45u 195u 60u "Start the modded game with the Rocketify shortcut on your desktop or in the Start menu.$\r$\n$\r$\nTo play online, start Rocket League normally from the Epic Games Launcher - it runs with Easy Anti-Cheat and without mods."
    Pop $0
    SetCtlColors $0 "" "ffffff"
  ${EndIf}

  ${NSD_CreateCheckbox} 120u 162u 195u 12u "Start Rocketify now"
  Pop $FinishRun
  SetCtlColors $FinishRun "" "ffffff"
FunctionEnd

Function OnCopyClick
  Call CopyLaunchOption
  ${NSD_SetText} $FinishCopy "Copied!"
FunctionEnd

Function FinishLeave
  ${NSD_GetState} $FinishRun $0
  ${If} $0 == ${BST_CHECKED}
    Exec '"$Win64Dir\Rocketify.exe"'
  ${EndIf}
FunctionEnd

Function CopyLaunchOption
  ${If} $Edition != "Steam"
    Return
  ${EndIf}
  StrCpy $0 '"$Win64Dir\Rocketify.exe" %command%'
  StrLen $1 $0
  IntOp $1 $1 + 1
  IntOp $1 $1 * 2
  System::Call 'user32::OpenClipboard(p 0) i'
  System::Call 'user32::EmptyClipboard() i'
  System::Call 'kernel32::GlobalAlloc(i 0x2, p r1) p.r2'
  System::Call 'kernel32::GlobalLock(p r2) p.r3'
  System::Call 'kernel32::lstrcpyW(p r3, w r0)'
  System::Call 'kernel32::GlobalUnlock(p r2)'
  System::Call 'user32::SetClipboardData(i 13, p r2) p'
  System::Call 'user32::CloseClipboard() i'
FunctionEnd

; ---------------------------------------------------------------- sections
Section "Mod loader (required)" SecCore
  SectionIn RO
  Call NormalizeWin64Dir   ; also covers silent installs (/S /D=...)

  ; Migrate an install made under the old names (RLModLauncher.exe / rlml\)
  ${If} ${FileExists} "$Win64Dir\RLModLauncher.exe"
  ${OrIf} ${FileExists} "$Win64Dir\rlml\*.*"
    DetailPrint "Removing the old RL Mod Loader files..."
    CreateDirectory "$Win64Dir\rocketify\mods"
    CopyFiles /SILENT "$Win64Dir\rlml\mods\*.dll" "$Win64Dir\rocketify\mods"
    Delete "$Win64Dir\rocketify\mods\hello_mod.dll"
    Delete "$Win64Dir\RLModLauncher.exe"
    RMDir /r "$Win64Dir\rlml"
    Delete "$DESKTOP\Rocket League (Mods).lnk"
    RMDir /r "$SMPROGRAMS\Rocket League Mods"
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\RLMods"
  ${EndIf}
  SetOutPath "$Win64Dir"
  File "${BUILD}/Rocketify.exe"
  SetOutPath "$Win64Dir\rocketify"
  File "${BUILD}/rocketify.dll"
  CreateDirectory "$Win64Dir\rocketify\mods"
  WriteUninstaller "$Win64Dir\rocketify\Uninstall Rocketify.exe"

  ; Apps & Features entry
  WriteRegStr HKLM "${REGKEY}" "DisplayName" "${APPNAME} ($Edition)"
  WriteRegStr HKLM "${REGKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${REGKEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr HKLM "${REGKEY}" "InstallLocation" "$Win64Dir\rocketify"
  WriteRegStr HKLM "${REGKEY}" "DisplayIcon" "$Win64Dir\RocketLeague.exe"
  WriteRegStr HKLM "${REGKEY}" "UninstallString" '"$Win64Dir\rocketify\Uninstall Rocketify.exe"'
  WriteRegDWORD HKLM "${REGKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${REGKEY}" "NoRepair" 1
  ${GetSize} "$Win64Dir\rocketify" "/S=0K" $0 $1 $2
  WriteRegDWORD HKLM "${REGKEY}" "EstimatedSize" $0
SectionEnd

Section "Example mod (Hello Mod)" SecExample
  SetOutPath "$Win64Dir\rocketify\mods"
  File "${BUILD}/hello_mod.dll"
SectionEnd

Section "Desktop shortcut" SecDesktop
  SetOutPath "$Win64Dir"
  CreateShortcut "$DESKTOP\Rocketify.lnk" "$Win64Dir\Rocketify.exe" "" "$Win64Dir\RocketLeague.exe" 0
SectionEnd

Section "Start menu shortcuts" SecStartMenu
  SetOutPath "$Win64Dir"
  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Rocketify.lnk" "$Win64Dir\Rocketify.exe" "" "$Win64Dir\RocketLeague.exe" 0
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Mods folder.lnk" "$Win64Dir\rocketify\mods"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$Win64Dir\rocketify\Uninstall Rocketify.exe"
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecCore} "The launcher and the mod loader (MODS menu entry, mod loading)."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecExample} "A tiny example mod that writes a line to the log."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A Rocketify shortcut on the desktop."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "Shortcuts to the modded game, the mods folder and the uninstaller."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---------------------------------------------------------------- uninstaller
Function un.onInit
  SetRegView 64
FunctionEnd

Section "Uninstall"
  ; $INSTDIR = ...\Binaries\Win64\rocketify
  ${GetParent} "$INSTDIR" $0
  Delete "$0\Rocketify.exe"
  Delete "$INSTDIR\rocketify.dll"
  Delete "$INSTDIR\rocketify.log"
  Delete "$INSTDIR\rocketify.ini"
  Delete "$INSTDIR\splash_trace.log"
  Delete "$INSTDIR\splash_curve.txt"
  Delete "$INSTDIR\mods\hello_mod.dll"
  RMDir "$INSTDIR\mods"          ; only if empty - your own mods are kept
  Delete "$INSTDIR\Uninstall Rocketify.exe"
  RMDir "$INSTDIR"

  Delete "$DESKTOP\Rocketify.lnk"
  RMDir /r "$SMPROGRAMS\${APPNAME}"
  DeleteRegKey HKLM "${REGKEY}"
SectionEnd
