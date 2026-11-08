; winapk.iss -- the Win.apk installer (Inno Setup 6).
;
;   iscc /DSourceDir=<staged folder with jre> /DAppVersion=0.1.0-preview windows\package\winapk.iss
;
; Installs per user (no administrator rights) to %LOCALAPPDATA%\Programs\Win.apk,
; adds Win.apk to the Start menu (and the desktop if asked), opens .apk files
; with it, and uninstalls cleanly. Apps and their data live in
; %LOCALAPPDATA%\Win.apk and stay when Win.apk is uninstalled.
;
; Setup speaks English or Hebrew, chosen from the Windows display language
; without asking (Hebrew is laid out right to left, from Inno Setup's own
; Languages\Hebrew.isl). The file is UTF-8 with a BOM, which Inno Setup needs
; to read the Hebrew below. Inno Setup lays the Hebrew wizard's controls out
; right to left but leaves its title bar as in English; [Code] mirrors that too
; (the icon on the right, Close on the left), as a Hebrew Windows draws it.

#ifndef SourceDir
  #error Pass /DSourceDir=<the staged Win.apk folder>
#endif
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
; The numeric part (0.2.0 of 0.2.0-preview), for the file's version resource.
#ifndef AppNumber
  #define AppNumber "0.1.0"
#endif

[Setup]
AppId={{6E1B7C2A-5F3D-4C8B-9A0E-7D2F1A4B3C59}
AppName=Win.apk
AppVersion={#AppVersion}
AppVerName=Win.apk {#AppVersion}
AppPublisher=Win.apk
VersionInfoVersion={#AppNumber}
DefaultDirName={localappdata}\Programs\Win.apk
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputBaseFilename=Win.apk-Setup
SetupIconFile=..\launcher\winapk.ico
UninstallDisplayIcon={app}\Win.apk.exe
UninstallDisplayName=Win.apk
WizardStyle=modern
; A fixed size: [Code] mirrors the Hebrew wizard's frame, and a resize would move its buttons back.
WizardResizable=no
Compression=lzma2/max
SolidCompression=yes
ChangesAssociations=yes
CloseApplications=yes
ShowLanguageDialog=no
LanguageDetectionMethod=uilanguage

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "hebrew"; MessagesFile: "compiler:Languages\Hebrew.isl"

[CustomMessages]
english.DesktopIcon=Put Win.apk on the desktop
hebrew.DesktopIcon=הוספת Win.apk לשולחן העבודה
english.AppComment=Android apps on Windows
hebrew.AppComment=אפליקציות Android ב-Windows
english.FileTypeName=Android app
hebrew.FileTypeName=אפליקציית Android
english.OpenWinApk=Open Win.apk
hebrew.OpenWinApk=פתיחת Win.apk

[Tasks]
Name: desktopicon; Description: "{cm:DesktopIcon}"

[InstallDelete]
; The Java runtime is replaced whole: files an older one had and this one has
; not (other modules' DLLs, a second CDS archive) would otherwise stay.
Type: filesandordirs; Name: "{app}\jre"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion createallsubdirs

[Icons]
Name: "{userprograms}\Win.apk"; Filename: "{app}\Win.apk.exe"; Comment: "{cm:AppComment}"
Name: "{userdesktop}\Win.apk"; Filename: "{app}\Win.apk.exe"; Comment: "{cm:AppComment}"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Classes\.apk"; ValueType: string; ValueData: "WinApk.Package"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\WinApk.Package"; ValueType: string; ValueData: "{cm:FileTypeName}"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\WinApk.Package\DefaultIcon"; ValueType: string; ValueData: """{app}\Win.apk.exe"",0"
Root: HKCU; Subkey: "Software\Classes\WinApk.Package\shell\open\command"; ValueType: string; ValueData: """{app}\Win.apk.exe"" ""%1"""

[UninstallDelete]
; The apps' shortcuts the library made (Start > Win.apk Apps).
Type: filesandordirs; Name: "{userprograms}\Win.apk Apps"

[Run]
Filename: "{app}\Win.apk.exe"; Description: "{cm:OpenWinApk}"; Flags: nowait postinstall skipifsilent

[Code]
// Inno Setup's right-to-left forms flip their controls but not the title bar:
// that takes WS_EX_LAYOUTRTL, which the VCL does not set (Setup.SetupForm.pas).
// Set here, with WS_EX_NOINHERITLAYOUT so the controls inside keep drawing
// themselves as they do, it mirrors the frame and the form's own coordinates.
// The form's direct children are then placed mirrored too, so they are flipped
// back first: InitializeWizard runs before Inno Setup flips the controls, and
// two flips leave them where the mirrored form shows them right to left.
// Pascal Script has no Anchors on TControl, so only their positions are
// flipped; the wizard keeps one size (WizardResizable=no), where anchors do
// not move anything.
const
  GWL_EXSTYLE = -20;
  WS_EX_LAYOUTRTL = $00400000;
  WS_EX_NOINHERITLAYOUT = $00100000;
  SWP_NOSIZE = $0001;
  SWP_NOMOVE = $0002;
  SWP_NOZORDER = $0004;
  SWP_FRAMECHANGED = $0020;

function GetWindowLongW(hWnd: HWND; nIndex: Integer): LongInt;
  external 'GetWindowLongW@user32.dll stdcall';
function SetWindowLongW(hWnd: HWND; nIndex: Integer; dwNewLong: LongInt): LongInt;
  external 'SetWindowLongW@user32.dll stdcall';
function SetWindowPos(hWnd: HWND; hWndInsertAfter: HWND; X, Y, cx, cy: Integer; uFlags: UINT): BOOL;
  external 'SetWindowPos@user32.dll stdcall';

procedure MirrorTitleBar(Form: TForm);
var
  I: Integer;
  C: TControl;
begin
  for I := 0 to Form.ControlCount - 1 do begin
    C := Form.Controls[I];
    C.Left := Form.ClientWidth - C.Left - C.Width;
  end;
  SetWindowLongW(Form.Handle, GWL_EXSTYLE,
    GetWindowLongW(Form.Handle, GWL_EXSTYLE) or WS_EX_LAYOUTRTL or WS_EX_NOINHERITLAYOUT);
  SetWindowPos(Form.Handle, 0, 0, 0, 0, 0, SWP_NOMOVE or SWP_NOSIZE or SWP_NOZORDER or SWP_FRAMECHANGED);
end;

procedure InitializeWizard;
begin
  if ActiveLanguage = 'hebrew' then MirrorTitleBar(WizardForm);
end;
