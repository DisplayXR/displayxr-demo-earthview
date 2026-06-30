; DisplayXR EarthView Demo — Windows Installer
; Copyright 2026, DisplayXR
; SPDX-License-Identifier: BSL-1.0
;
; Build: makensis /DVERSION=1.2.0 /DBIN_DIR=<demo-build-dir> /DSOURCE_DIR=<demo-repo-root> /DOUTPUT_DIR=<output-dir> DisplayXREarthViewInstaller.nsi
;
; Hard-prereqs the DisplayXR runtime (HKLM\Software\DisplayXR\Runtime\InstallPath).
; Installs the demo exe + the Google attribution logo to
; Program Files\DisplayXR\Demos\EarthView\. Drops a registered-mode app manifest
; + icons under %ProgramData%\DisplayXR\apps\ so the DisplayXR Shell launcher
; discovers the tile (system-wide, since the installer runs elevated).
;
; SECURITY (docs/api-key.md): the installer NEVER stages a Google Map Tiles API
; key. No earthview.ini / *.key is bundled — the end user supplies their own key
; via env / %APPDATA%\DisplayXR\EarthView\earthview.ini. CI asserts the payload
; contains no key file.

!ifndef VERSION
    !define VERSION "1.0.0"
!endif
!ifndef VERSION_MAJOR
    !define VERSION_MAJOR "1"
!endif
!ifndef VERSION_MINOR
    !define VERSION_MINOR "0"
!endif
!ifndef VERSION_PATCH
    !define VERSION_PATCH "0"
!endif

!ifndef BIN_DIR
    !define BIN_DIR "${__FILEDIR__}\..\..\build\windows"
!endif
!ifndef SOURCE_DIR
    !define SOURCE_DIR "${__FILEDIR__}\..\.."
!endif
!ifndef OUTPUT_DIR
    !define OUTPUT_DIR "${__FILEDIR__}"
!endif

;--------------------------------
; Code signing (SIGN_CMD passed from build-installer.bat; empty/absent =
; unsigned build). SIGN_CMD carries no secret — on a signing-capable build
; machine it points at the configured signer; elsewhere it is absent and the
; build is unsigned.
;
; The installer .exe is signed via !finalize. The UNINSTALLER is signed via a
; two-pass build instead of !uninstfinalize (which is unreliable — the
; uninstaller NSIS writes at install time inherits the signed installer's
; cert-table pointer, which can dangle past the smaller uninstaller file =>
; effectively unsigned): compile an INNER installer whose only job is to
; WriteUninstaller to %TEMP% and Quit; run it; sign that %TEMP%\Uninstall.exe;
; then File-include the pre-signed uninstaller in the real pass. INNER is
; RequestExecutionLevel user so it never triggers UAC from makensis.
!ifndef INNER
    !ifdef SIGN_CMD
        !if "${SIGN_CMD}" != ""
            !finalize '${SIGN_CMD} "%1"'
            !makensis '-DINNER "-DVERSION=${VERSION}" "-DVERSION_MAJOR=${VERSION_MAJOR}" "-DVERSION_MINOR=${VERSION_MINOR}" "-DVERSION_PATCH=${VERSION_PATCH}" "-DSOURCE_DIR=${SOURCE_DIR}" "-DBIN_DIR=${BIN_DIR}" "-DOUTPUT_DIR=${OUTPUT_DIR}" "${__FILE__}"' = 0
            !system '"$%TEMP%\DisplayXREarthViewSetup_inner.exe"' = 2
            !system '${SIGN_CMD} "$%TEMP%\Uninstall.exe"' = 0
            !define USE_PRESIGNED_UNINST
        !endif
    !endif
!endif

;--------------------------------
; General

Name "DisplayXR EarthView Demo ${VERSION}"
!ifdef INNER
    ; Throwaway inner installer: only emits the uninstaller to %TEMP%.
    OutFile "$%TEMP%\DisplayXREarthViewSetup_inner.exe"
    RequestExecutionLevel user
!else
    OutFile "${OUTPUT_DIR}\DisplayXREarthViewSetup-${VERSION}.exe"
    RequestExecutionLevel admin
!endif
InstallDir "$PROGRAMFILES64\DisplayXR\Demos\EarthView"
InstallDirRegKey HKLM "Software\DisplayXR\Demos\EarthView" "InstallPath"
ShowInstDetails show
ShowUninstDetails show

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!include "WordFunc.nsh"
!insertmacro VersionCompare

; Minimum runtime version. EarthView relies on XR_EXT_view_rig (the runtime owns
; the off-axis Kooima + window resolve), XR_EXT_display_info, and
; XR_EXT_atlas_capture — the same #396 consume path the modelviewer demo uses
; against runtime ≥ 1.3.0. Older runtimes lack the render-ready XrView{pose,fov}
; channel the tile renderer draws through, so the scene won't appear.
!define MIN_RUNTIME_VERSION "1.3.0"

;--------------------------------
; UI

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "DisplayXR EarthView Demo Setup"
!define MUI_WELCOMEPAGE_TEXT "This will install the EarthView reference demo for the DisplayXR runtime.$\r$\n$\r$\nEarthView streams Google Photorealistic 3D Tiles and needs your own Google Map Tiles API key at runtime.$\r$\n$\r$\nThe DisplayXR runtime must be installed first."

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

;--------------------------------
; Pre-flight: hard-prereq the runtime

Function .onInit
!ifdef INNER
    ; Inner pass only: emit the uninstaller to %TEMP% (the binary that gets
    ; signed and File-included by the real pass) then bail — no UI, no install.
    ; This whole path is absent from the real installer.
    SetSilent silent
    WriteUninstaller "$%TEMP%\Uninstall.exe"
    Quit
!endif
    ${IfNot} ${RunningX64}
        MessageBox MB_ICONSTOP "DisplayXR requires 64-bit Windows."
        Abort
    ${EndIf}

    ; HKLM\Software\DisplayXR\Runtime\InstallPath is set by the runtime
    ; installer's "DisplayXR Runtime" section. NSIS is a 32-bit executable so
    ; HKLM access is silently redirected through WOW6432Node by default; the
    ; runtime writes to the 64-bit view. Switch to 64-bit view to match.
    SetRegView 64
    ReadRegStr $0 HKLM "Software\DisplayXR\Runtime" "InstallPath"
    ReadRegStr $1 HKLM "Software\DisplayXR\Runtime" "Version"
    SetRegView 32
    ${If} $0 == ""
        MessageBox MB_ICONSTOP "DisplayXR runtime is not installed.$\r$\n$\r$\nInstall the DisplayXR runtime first, then re-run this installer.$\r$\n$\r$\nGet it from:$\r$\nhttps://github.com/DisplayXR/displayxr-runtime/releases"
        Abort
    ${EndIf}

    ; Enforce the minimum runtime version for the XR_EXT_view_rig consume path
    ; this demo renders through. The Leia SR Vulkan weaver DLL is the Leia
    ; plug-in's concern (it ships + loads SimulatedRealityVulkanBeta.dll itself)
    ; — the demo neither bundles nor depends on it being on PATH.
    ${VersionCompare} "$1" "${MIN_RUNTIME_VERSION}" $2
    ${If} $2 == 2
        MessageBox MB_ICONSTOP "DisplayXR runtime $1 is too old.$\r$\n$\r$\nThis demo requires runtime ${MIN_RUNTIME_VERSION} or later.$\r$\n$\r$\nUpdate from:$\r$\nhttps://github.com/DisplayXR/displayxr-runtime/releases"
        Abort
    ${EndIf}
FunctionEnd

;--------------------------------
; Install

Section "EarthView Demo" SecDemo
    SectionIn RO

    ; Match the runtime installer's 64-bit registry view so HKLM keys land
    ; in the canonical (non-WOW6432Node) hive.
    SetRegView 64

    ; All-users context — $APPDATA -> %ProgramData%, $SMPROGRAMS -> All Users.
    SetShellVarContext all

    ; Kill any running instance so we can overwrite the exe.
    nsExec::ExecToLog 'taskkill /f /im earthview_handle_vk_win.exe'
    Pop $0

    SetOutPath "$INSTDIR"
    File "${BIN_DIR}\earthview_handle_vk_win.exe"

    ; Google attribution logo — REQUIRED next to the exe (Map Tiles API policy,
    ; PRD §7.3). The build stages it via windows/CMakeLists.txt POST_BUILD.
    File "${BIN_DIR}\google_logo.png"

    ; OpenXR loader — an OpenXR app must carry its own openxr_loader.dll next
    ; to the exe. The runtime ships a copy in its install dir, but that dir is
    ; intentionally not on PATH and not part of an app's DLL search order, so a
    ; demo installed under Demos\EarthView\ can't find it there. The Windows
    ; build stages it next to the exe (windows/CMakeLists.txt POST_BUILD + the
    ; CI loader-stage step).
    File "${BIN_DIR}\openxr_loader.dll"

    ; NOTE: deliberately NO earthview.ini / *.key in the payload — the API key
    ; is the end user's, supplied at runtime (docs/api-key.md). CI asserts this.

    ; Drop the registered-mode app manifest + icons under %ProgramData%
    ; (system-wide, installer-elevated — matches §2.2 of the manifest spec).
    CreateDirectory "$APPDATA\DisplayXR\apps"
    SetOutPath "$APPDATA\DisplayXR\apps"

    ; Per-app icon names — the apps dir is shared by all registered demos, so
    ; generic icon.png/icon_sbs.png would collide. Keep these prefixed + unique.
    File "${SOURCE_DIR}\windows\displayxr\earthview_icon.png"
    File "${SOURCE_DIR}\windows\displayxr\earthview_icon_sbs.png"

    ; Generate the manifest with an absolute exe_path pointing at the install
    ; dir we just populated (registered-mode; the in-tree sidecar is sidecar-mode
    ; and omits exe_path).
    FileOpen $0 "$APPDATA\DisplayXR\apps\earthview.displayxr.json" w
    FileWrite $0 '{$\r$\n'
    FileWrite $0 '  "schema_version": 1,$\r$\n'
    FileWrite $0 '  "id": "earthview",$\r$\n'
    FileWrite $0 '  "name": "EarthView",$\r$\n'
    FileWrite $0 '  "type": "3d",$\r$\n'
    FileWrite $0 '  "category": "demo",$\r$\n'
    FileWrite $0 '  "display_mode": "auto",$\r$\n'
    FileWrite $0 '  "description": "Streaming 3D city viewer on Google Photorealistic 3D Tiles. Fly the full-scale world camera-style, or double-click a landmark to inspect it and orbit around it. Requires a Google Map Tiles API key.",$\r$\n'
    FileWrite $0 '  "icon": "earthview_icon.png",$\r$\n'
    FileWrite $0 '  "icon_3d": "earthview_icon_sbs.png",$\r$\n'
    FileWrite $0 '  "icon_3d_layout": "sbs-lr",$\r$\n'
    ; Forward slashes in exe_path so the JSON parses with any strict library.
    ${WordReplace} "$INSTDIR" "\" "/" "+" $1
    FileWrite $0 '  "exe_path": "$1/earthview_handle_vk_win.exe"$\r$\n'
    FileWrite $0 '}$\r$\n'
    FileClose $0

    ; Registry breadcrumbs.
    SetRegView 64
    WriteRegStr HKLM "Software\DisplayXR\Demos\EarthView" "InstallPath" "$INSTDIR"
    WriteRegStr HKLM "Software\DisplayXR\Demos\EarthView" "Version" "${VERSION}"

    ; Add/Remove Programs entry.
    ; In a signed build, install the pre-signed uninstaller produced by the
    ; inner pass (two-pass signing — see the code-signing block in the header);
    ; otherwise (unsigned build / CI) write it normally. File /oname respects
    ; the current $OUTDIR (set to $APPDATA\DisplayXR\apps above), so point it
    ; back at $INSTDIR where the UninstallString entries below expect it.
!ifdef USE_PRESIGNED_UNINST
    SetOutPath "$INSTDIR"
    File "/oname=Uninstall.exe" "$%TEMP%\Uninstall.exe"
!else
    WriteUninstaller "$INSTDIR\Uninstall.exe"
!endif
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXREarthView" \
        "DisplayName" "DisplayXR EarthView Demo"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXREarthView" \
        "UninstallString" "$\"$INSTDIR\Uninstall.exe$\""
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXREarthView" \
        "QuietUninstallString" "$\"$INSTDIR\Uninstall.exe$\" /S"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXREarthView" \
        "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXREarthView" \
        "DisplayIcon" "$INSTDIR\earthview_handle_vk_win.exe"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXREarthView" \
        "Publisher" "DisplayXR"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXREarthView" \
        "DisplayVersion" "${VERSION}"
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXREarthView" \
        "VersionMajor" ${VERSION_MAJOR}
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXREarthView" \
        "VersionMinor" ${VERSION_MINOR}
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXREarthView" \
        "NoModify" 1
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXREarthView" \
        "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    IntFmt $0 "0x%08X" $0
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXREarthView" \
        "EstimatedSize" "$0"
SectionEnd

Section "Start Menu Shortcut" SecShortcut
    SetShellVarContext all
    CreateDirectory "$SMPROGRAMS\DisplayXR"
    CreateShortCut "$SMPROGRAMS\DisplayXR\EarthView.lnk" \
        "$INSTDIR\earthview_handle_vk_win.exe" "" \
        "$INSTDIR\earthview_handle_vk_win.exe" 0
SectionEnd

;--------------------------------
; Uninstall

Section "Uninstall"
    SetRegView 64
    SetShellVarContext all

    nsExec::ExecToLog 'taskkill /f /im earthview_handle_vk_win.exe'
    Pop $0

    ; The DisplayXR Shell periodically scans %ProgramData%\DisplayXR\apps\ and
    ; may hold an open handle to earthview.displayxr.json, which makes the Delete
    ; below silently fail. Kill it first; the user re-invokes the shell to get it
    ; back. Sleep 500 gives Windows a moment to release the handle.
    DetailPrint "Stopping DisplayXR Shell to release manifest handles..."
    nsExec::ExecToLog 'taskkill /f /im displayxr-shell.exe'
    Pop $0
    Sleep 500

    ; Remove the registered-mode manifest + icons.
    Delete /REBOOTOK "$APPDATA\DisplayXR\apps\earthview.displayxr.json"
    Delete /REBOOTOK "$APPDATA\DisplayXR\apps\earthview_icon.png"
    Delete /REBOOTOK "$APPDATA\DisplayXR\apps\earthview_icon_sbs.png"
    RMDir "$APPDATA\DisplayXR\apps"

    ; Remove install dir contents.
    Delete "$INSTDIR\earthview_handle_vk_win.exe"
    Delete "$INSTDIR\google_logo.png"
    Delete "$INSTDIR\openxr_loader.dll"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    RMDir "$PROGRAMFILES64\DisplayXR\Demos"

    ; Start menu shortcut.
    Delete "$SMPROGRAMS\DisplayXR\EarthView.lnk"
    ; Don't RMDir $SMPROGRAMS\DisplayXR — the runtime's own shortcuts may
    ; still live there.

    DeleteRegKey HKLM "Software\DisplayXR\Demos\EarthView"
    DeleteRegKey /ifempty HKLM "Software\DisplayXR\Demos"
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXREarthView"
SectionEnd

;--------------------------------
; Version metadata

VIProductVersion "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}.0"
VIAddVersionKey "ProductName" "DisplayXR EarthView Demo"
VIAddVersionKey "CompanyName" "DisplayXR"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2026 DisplayXR"
VIAddVersionKey "FileDescription" "DisplayXR EarthView Demo Installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
