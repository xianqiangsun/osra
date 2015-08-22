!define DOT_VERSION  "2.0.1"
!define DASH_VERSION "2-0-1"

;!define GS_EXE "gs916w32.exe"
;!define GS_URL "http://downloads.ghostscript.com/public/${GS_EXE}"
!define GS_EXE "gs907w32.exe"
!define GS_URL "http://downloads.ghostscript.com/public/${GS_EXE}"

!include Sections.nsh
; include for some of the windows messages defines
!include "winmessages.nsh"
; HKLM (all users) vs HKCU (current user) defines
!define env_hklm 'HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"'
!define env_hkcu 'HKCU "Environment"'

!include "TextFunc.nsh" 
!insertmacro LineFind

; The name of the installer
Name "Optical Structure Recognition Application"

; The file to write
OutFile "osra-setup-${DASH_VERSION}.exe"

; The default installation directory
InstallDir $PROGRAMFILES\osra\${DOT_VERSION}

; Registry key to check for directory (so if you install again, it will 
; overwrite the old one automatically)
InstallDirRegKey HKLM "Software\osra\${DOT_VERSION}" "Install_Dir"

LicenseData "license.txt"

; Request application privileges for Windows Vista
RequestExecutionLevel admin

;--------------------------------

; Pages

Page license
Page components
Page directory
Page instfiles

UninstPage uninstConfirm
UninstPage instfiles

;--------------------------------

; The stuff to install
Section "osra (required)"

  SectionIn RO
  
  ; Set output path to the installation directory.
  SetOutPath $INSTDIR
  
  ; Put file there
  File "osra-bin.exe"
  File "pthreadGC2.dll"
  File "delegates.mgk"
  File "type.mgk"
  File "type-ghostscript.mgk"
  File "type-solaris.mgk"
  File "type-windows.mgk"
  File "colors.mgk"
  File "log.mgk"
  File "magic.mgk"
  File "modules.mgk"
  File "README.txt"
  File "spelling.txt"
  File "superatom.txt"
  strcpy $3 "GPL Ghostscript"
  call checkSoftVersion
  call getGhostscriptInstPath
  strcmp $1 "" no_gs +2
  no_gs:
  MessageBox MB_OK "Ghostscript interpreter not found" IDOK 0 
  call createOSRAbat
  
  
  ; Write the installation path into the registry
  WriteRegStr HKLM SOFTWARE\osra\${DOT_VERSION} "Install_Dir" "$INSTDIR"
  WriteRegStr HKLM SOFTWARE\osra "Install_Dir" "$INSTDIR"
  
  ; Write the uninstall keys for Windows
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\osra" "DisplayName" "OSRA ${DOT_VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\osra" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\osra" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\osra" "NoRepair" 1
  WriteUninstaller "uninstall.exe"
  
  
  ; set variable
  WriteRegExpandStr ${env_hklm} OSRA "$INSTDIR"
  ; make sure windows knows about the change
  SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000

SectionEnd

Section /o "Accelrys Draw plugin" symyx_draw
 call getSymyxPath
 strcmp $1 "" no_symyx
 SetOutPath "$1\AddIns"
 File "plugins\symyx_draw\OSRAAction.xml"
 SetOutPath "$1\AddIns\OSRAAction"
 File "plugins\symyx_draw\OSRAAction\README.txt"
 File "plugins\symyx_draw\OSRAAction\OSRAAction.dll"
 Goto done
 no_symyx:
  MessageBox MB_OK "Accelrys Draw not found" IDOK done
 done:
SectionEnd


; Uninstaller

Section "Uninstall"
	# call userInfo plugin to get user info.  The plugin puts the result in the stack
    userInfo::getAccountType
   
    # pop the result from the stack into $0
    pop $0
 
    # compare the result with the string "Admin" to see if the user is admin.
    # If match, jump 3 lines down.
    strCmp $0 "Admin" +3
 
    # if there is not a match, print message and return
    messageBox MB_OK "Please run this with Administrator privileges"
    Quit   
  ReadRegStr $0 HKLM SOFTWARE\osra\${DOT_VERSION} "Install_Dir"
  strcpy $INSTDIR $0
  ; Remove registry keys
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\osra"
  DeleteRegKey HKLM SOFTWARE\osra\${DOT_VERSION}
  ; delete variable
  DeleteRegValue ${env_hklm} OSRA
  ; make sure windows knows about the change
  SendMessage ${HWND_BROADCAST} ${WM_WININICHANGE} 0 "STR:Environment" /TIMEOUT=5000

  ; Remove files and uninstaller
  Delete $INSTDIR\osra-bin.exe
  Delete $INSTDIR\pthreadGC2.dll
  Delete $INSTDIR\delegates.mgk
  Delete $INSTDIR\type.mgk
  Delete $INSTDIR\type-ghostscript.mgk
  Delete $INSTDIR\type-solaris.mgk
  Delete $INSTDIR\type-windows.mgk
  Delete $INSTDIR\colors.mgk
  Delete $INSTDIR\log.mgk
  Delete $INSTDIR\magic.mgk
  Delete $INSTDIR\modules.mgk
  Delete $INSTDIR\README.txt
  Delete $INSTDIR\osra.bat
  Delete $INSTDIR\superatom.txt
  Delete $INSTDIR\spelling.txt
  Delete $INSTDIR\uninstall.exe
  RMDir "$INSTDIR"
  call un.getSymyxPath
  strcmp $1 "" no_symyx 
  Delete "$1\AddIns\OSRAAction.xml"
  Delete "$1\AddIns\OSRAAction\README.txt"
  Delete "$1\AddIns\OSRAAction\OSRAAction.dll"
  Delete "$1\AddIns\OSRAAction\OSRAAction.dll.config"
  RMDir "$1\AddIns\OSRAAction"
  no_symyx:
SectionEnd

Function CheckSoftVersion
StrCpy $0 0
StrCpy $2 ""
loop:
  EnumRegKey $1 HKLM "Software\$3" $0
  StrCmp $1 "" done
  StrCpy $2 $1
  IntOp $0 $0 + 1
  Goto loop
done:
; $2 contains the version of Soft now or empty
FunctionEnd


Function getGhostscriptInstPath
 strcmp $2 "" download_gs get_path
 download_gs:
   DetailPrint "need to download and install Ghostscript"
   Call ConnectInternet ;Make an internet connection (if no connection available)
   StrCpy $2 "$TEMP\${GS_EXE}"
   NSISdl::download ${GS_URL} $2
   Pop $0
   StrCmp $0 success success
    SetDetailsView show
    DetailPrint "download failed: $0"
    Abort
   success:
    ExecWait "$2"
    Delete $2
    strcpy $2 "9.07"
	
 get_path:
  strcpy $1 ""
  ReadRegStr $0 HKLM \
     "Software\GPL Ghostscript\$2" \ 
     "GS_DLL"
  StrCmp $0 "" fin extract
  
 extract:
  StrCpy $1 $0 -16
  IfFileExists $1\bin\gswin32c.exe fin
  StrCpy $1 ""
  fin:
  ;$1 contains the folder of Ghostscript or empty
FunctionEnd

Function getSymyxPath
  Push "$PROGRAMFILES\Accelrys"
  Push "AccelrysDraw.exe"
  Call FindIt
  Pop $R1
  Push "$R1"
  Call GetParent
  Pop $R0
  StrCpy $1 "$R0\"
  IfFileExists $1AccelrysDraw.exe fin
  Push "$PROGRAMFILES\Symyx"
  Push "SymyxDraw.exe"
  Call FindIt
  Pop $R1
  Push "$R1"
  Call GetParent
  Pop $R0
  StrCpy $1 "$R0\"
  IfFileExists $1SymyxDraw.exe fin
  StrCpy $1 ""	
  fin:
  ;$1 contains the folder of Symyx Draw or empty
FunctionEnd


Function un.getSymyxPath
  Push "$PROGRAMFILES\Accelrys"
  Push "AccelrysDraw.exe"
  Call un.FindIt
  Pop $R1
  Push "$R1"
  Call un.GetParent
  Pop $R0
  StrCpy $1 "$R0\"
  IfFileExists $1AccelrysDraw.exe fin
  Push "$PROGRAMFILES\Symyx"
  Push "SymyxDraw.exe"
  Call un.FindIt
  Pop $R1
  Push "$R1"
  Call un.GetParent
  Pop $R0
  StrCpy $1 "$R0\"
  IfFileExists $1SymyxDraw.exe fin
  StrCpy $1 ""	
  fin:
  ;$1 contains the folder of Symyx Draw or empty
FunctionEnd


Function createOSRAbat
fileOpen $0 "$INSTDIR\osra.bat" w
  fileWrite $0 '\
@echo off$\r$\n\
setlocal$\r$\n\
set exec_dir=%~dp0%$\r$\n\
set PATH=%exec_dir%;$1\bin;$1\lib;%PATH%$\r$\n\
set MAGICK_CONFIGURE_PATH=%exec_dir%$\r$\n\
"%exec_dir%osra-bin.exe" %*$\r$\n\
endlocal$\r$\n\
'
fileClose $0
FunctionEnd


Function ConnectInternet

  Push $R0
    
    ClearErrors
    Dialer::AttemptConnect
    IfErrors noie3
    
    Pop $R0
    StrCmp $R0 "online" connected
      MessageBox MB_OK|MB_ICONSTOP "Cannot connect to the internet."
      Quit
    
    noie3:
  
    ; IE3 not installed
    MessageBox MB_OK|MB_ICONINFORMATION "Please connect to the internet now."
    
    connected:
  
  Pop $R0
  
FunctionEnd


Function .onInit
	# call userInfo plugin to get user info.  The plugin puts the result in the stack
    userInfo::getAccountType
   
    # pop the result from the stack into $0
    pop $0
 
    # compare the result with the string "Admin" to see if the user is admin.
    # If match, jump 3 lines down.
    strCmp $0 "Admin" +3
 
    # if there is not a match, print message and return
    messageBox MB_OK "Please run this with Administrator privileges"
    Quit
 call getSymyxPath
 strcmp $1 "" no_symyx
 SectionGetFlags "${symyx_draw}" $0
 IntOp $0 $0 | ${SF_SELECTED}
 SectionSetFlags "${symyx_draw}" $0
 no_symyx:
FunctionEnd

Function FindIt
Exch $R0
Exch
Exch $R1
Push $R2
Push $R3
Push $R4
Push $R5
Push $R6
 
 StrCpy $R6 -1
 StrCpy $R3 1
 
 Push $R1
 
 nextDir:
  Pop $R1
  IntOp $R3 $R3 - 1
  ClearErrors
   FindFirst $R5 $R2 "$R1\*.*"
 
 nextFile:
  StrCmp $R2 "." gotoNextFile
  StrCmp $R2 ".." gotoNextFile
 
  StrCmp $R2 $R0 0 isDir
   StrCpy $R6 "$R1\$R2"
   loop:
    StrCmp $R3 0 done
     Pop $R1
     IntOp $R3 $R3 - 1
     Goto loop
 
 isDir:
 
  IfFileExists "$R1\$R2\*.*" 0 gotoNextFile
  IntOp $R3 $R3 + 1
  Push "$R1\$R2"
 
 gotoNextFile:
  FindNext $R5 $R2
  IfErrors 0 nextFile
 
 done:
  FindClose $R5
  StrCmp $R3 0 0 nextDir
  StrCpy $R0 $R6
 
Pop $R6
Pop $R5
Pop $R4
Pop $R3
Pop $R2
Pop $R1
Exch $R0
FunctionEnd

Function GetParent
 
  Exch $R0
  Push $R1
  Push $R2
  Push $R3
 
  StrCpy $R1 0
  StrLen $R2 $R0
 
  loop:
    IntOp $R1 $R1 + 1
    IntCmp $R1 $R2 get 0 get
    StrCpy $R3 $R0 1 -$R1
    StrCmp $R3 "\" get
  Goto loop
 
  get:
    StrCpy $R0 $R0 -$R1
 
    Pop $R3
    Pop $R2
    Pop $R1
    Exch $R0
 
FunctionEnd

Function un.FindIt
Exch $R0
Exch
Exch $R1
Push $R2
Push $R3
Push $R4
Push $R5
Push $R6
 
 StrCpy $R6 -1
 StrCpy $R3 1
 
 Push $R1
 
 nextDir:
  Pop $R1
  IntOp $R3 $R3 - 1
  ClearErrors
   FindFirst $R5 $R2 "$R1\*.*"
 
 nextFile:
  StrCmp $R2 "." gotoNextFile
  StrCmp $R2 ".." gotoNextFile
 
  StrCmp $R2 $R0 0 isDir
   StrCpy $R6 "$R1\$R2"
   loop:
    StrCmp $R3 0 done
     Pop $R1
     IntOp $R3 $R3 - 1
     Goto loop
 
 isDir:
 
  IfFileExists "$R1\$R2\*.*" 0 gotoNextFile
  IntOp $R3 $R3 + 1
  Push "$R1\$R2"
 
 gotoNextFile:
  FindNext $R5 $R2
  IfErrors 0 nextFile
 
 done:
  FindClose $R5
  StrCmp $R3 0 0 nextDir
  StrCpy $R0 $R6
 
Pop $R6
Pop $R5
Pop $R4
Pop $R3
Pop $R2
Pop $R1
Exch $R0
FunctionEnd

Function un.GetParent
 
  Exch $R0
  Push $R1
  Push $R2
  Push $R3
 
  StrCpy $R1 0
  StrLen $R2 $R0
 
  loop:
    IntOp $R1 $R1 + 1
    IntCmp $R1 $R2 get 0 get
    StrCpy $R3 $R0 1 -$R1
    StrCmp $R3 "\" get
  Goto loop
 
  get:
    StrCpy $R0 $R0 -$R1
 
    Pop $R3
    Pop $R2
    Pop $R1
    Exch $R0
 
FunctionEnd