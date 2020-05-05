!define VERSION "$%APPVEYOR_BUILD_VERSION%"

!include "MUI.nsh"
!include "FileFunc.nsh"

Name "Open Inspection System"
OutFile "ois-installer-v${VERSION}.exe"

InstallDir "C:\ois"

!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function InstallService
!include "lib\nsis-nssm\InstallService.nsi"
FunctionEnd

Function UninstallService
!include "lib\nsis-nssm\UninstallService.nsi"
FunctionEnd

Function un.UninstallService
!include "lib\nsis-nssm\UninstallService.nsi"
FunctionEnd

Function .onInit
	# Do we have a previous installation?
	ReadRegStr $0 HKLM "Software\Tomaattinen\OpenInspectionSystem" "InstallDir"
	IfErrors +2 # Apparently not - but that's fine!
	StrCpy $INSTDIR $0
	ClearErrors
FunctionEnd

Section "Main" Main

	!define NSSM_EXECUTABLE "$INSTDIR\x64\nssm.exe"

	Push ois_AlliedVision
	Push ${NSSM_EXECUTABLE}
	Call UninstallService

	Push ois_ImageStorage
	Push ${NSSM_EXECUTABLE}
	Call UninstallService
	
	CreateDirectory "$INSTDIR"

	WriteUninstaller "$INSTDIR\ois-uninstaller.exe"

	WriteRegStr HKLM "Software\Tomaattinen\OpenInspectionSystem" "InstallDir" "$INSTDIR"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenInspectionSystem" "DisplayName" "OpenInspectionSystem"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenInspectionSystem" "UninstallString" "$\"$INSTDIR\ois-uninstaller.exe$\""
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenInspectionSystem" "Publisher" "Tomaattinen"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenInspectionSystem" "DisplayVersion" "${VERSION}"

	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenInspectionSystem" "EstimatedSize" "100000"

	SetOutPath "$INSTDIR\x64"
	File "lib\nssm.exe"
	File "x64\Release\*.exe"
	File "x64\Release\*.dll"

	SetOutPath "$INSTDIR\AlliedVision"
	File "cameras\alliedvision\VimbaParametersExample.ini"

	Push "" # Command line arguments
	Push "$INSTDIR\AlliedVision"
	Push "$INSTDIR\x64\AlliedVision.exe"
	Push ois_AlliedVision # Service name
	Push ${NSSM_EXECUTABLE}
	Call InstallService

	CreateDirectory "$INSTDIR\ImageStorage"
	Push "" # Command line arguments
	Push "$INSTDIR\ImageStorage"
	Push "$INSTDIR\x64\ImageStorage.exe"
	Push ois_ImageStorage # Service name
	Push ${NSSM_EXECUTABLE}
	Call InstallService
	
	CreateDirectory "$SMPROGRAMS\OpenInspectionSystem"
	CreateDirectory "$DESKTOP\OpenInspectionSystem"
	CreateDirectory "$INSTDIR\ImageViewer"

	SetOutPath "$INSTDIR\ImageViewer"
	CreateShortCut "$INSTDIR\ImageViewer.lnk" "$INSTDIR\x64\ImageViewer.exe" ""
	CreateShortCut "$SMPROGRAMS\OpenInspectionSystem\ImageViewer.lnk" "$INSTDIR\x64\ImageViewer.exe" ""
	CreateShortCut "$DESKTOP\OpenInspectionSystem\ImageViewer.lnk" "$INSTDIR\x64\ImageViewer.exe" ""

	SetOutPath "$INSTDIR"
SectionEnd

Section "Uninstall"

	Push ois_AlliedVision
	Push ${NSSM_EXECUTABLE}
	Call un.UninstallService
	
	Push ois_ImageStorage
	Push ${NSSM_EXECUTABLE}
	Call un.UninstallService

	DeleteRegKey HKLM "Software\Tomaattinen\OpenInspectionSystem"
	DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\OpenInspectionSystem"

	Delete "$INSTDIR\ImageViewer.lnk"
	Delete "$SMPROGRAMS\OpenInspectionSystem\ImageViewer.lnk"
	Delete "$DESKTOP\OpenInspectionSystem\ImageViewer.lnk"
	RMDir "$SMPROGRAMS\OpenInspectionSystem"
	RMDir "$DESKTOP\OpenInspectionSystem"
	
	RMDir /r $INSTDIR\ImageStorage\data
	
	Delete $INSTDIR\ImageStorage\*.*
	Delete $INSTDIR\ImageViewer\*.*
	Delete $INSTDIR\AlliedVision\*.*

	Delete $INSTDIR\x64\*.*
	Delete $INSTDIR\OpenInspectionSystem.lnk

	RMDir $INSTDIR\x64
	RMDir $INSTDIR\ImageStorage
	RMDir $INSTDIR\ImageViewer
	RMDir $INSTDIR\AlliedVision

	Delete $INSTDIR\nssm.exe
	Delete $INSTDIR\ois-uninstaller.exe

	SetOutPath ".."
	RMDir $INSTDIR

SectionEnd
