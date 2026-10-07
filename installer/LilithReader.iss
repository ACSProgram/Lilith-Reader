; LilithReader.iss — Lilith Reader 安装包脚本（Inno Setup 6.5+）
;
; 构建：运行 build_installer.bat（需 Inno Setup 6，winget install JRSoftware.InnoSetup）
; 产出：dist\LilithReader-<版本>-setup.exe
;
; ── 设计要点（每一条都对应一个真实取舍，改动前请先读） ─────────────────────────
;
; 1) **每用户安装**（PrivilegesRequired=lowest，不提供"为所有用户安装"）：
;    关联按 Windows 官方推荐写 HKCU；而管理员模式安装时 HKCU 指向的是提权后的
;    管理员账户，会把关联写到错的用户下。更要紧的是：本程序把 LilithReader.ini
;    与 reader_state.bin 写在 exe 同目录，装在 Program Files 下普通权限根本写不进去，
;    设置与阅读进度会静默失效。所以安装目录必须是当前用户可写的目录。
; 2) **升级是"就地覆盖"，不是"先卸载再装"**：Inno 认同一个 AppId，直接覆盖文件并
;    沿用上次的安装目录与任务勾选。刻意不做"检测到旧版→静默调用旧卸载器"——那会
;    把旧卸载器的"是否删除数据"弹窗插到升级流程中间（用户实测反馈的"机械性重复询问"）。
; 3) **卸载只问一次数据去留**：仅在数据文件确实存在、且非静默卸载时询问，
;    用任务对话框给出明确按钮（保留/一并删除），默认"保留"。
; 4) **运行中检测交给 CloseApplications**（Windows 重启管理器），不用改名试探之类的偏方。
; 5) 图标放进带版本号的子目录：Explorer 的图标缓存以"路径+索引"为键，路径含版本号
;    可保证升级后缓存自然失效。
;
; ──────────────────────────────────────────────────────────────────────────────

#define MyAppName "Lilith Reader"
; 显示版本（可含预发布后缀，用于文件名、注册表显示、欢迎页文案）
#define MyAppVersion "1.0-rc.1"
; 数字版本：PE 版本资源是 4 段纯数字，无法表达 "-rc.1"；它必须与
; src/app/app.rc 的 FILEVERSION 一致，是编译期校验的比对基准。
#define MyAppVersionNumeric "1.0.0.0"
#define MyAppPublisher "Lilith"
#define MyAppExeName "LilithReader.exe"
#define MyAppDescription "轻量高效的文档阅读器：PDF / EPUB / MOBI / FB2 / CBZ / XPS 与常见图片格式，单文件、无需额外运行库"
; 固定 AppId：升级/卸载识别的唯一凭据，发布后不得更改
#define MyAppId "{A6E4B7C2-9F31-4D08-B7A5-3C8E1D2F4A90}"

; 编译期版本一致性校验：exe 的版本资源（src/app/app.rc 的 FILEVERSION）必须与
; MyAppVersionNumeric 一致，否则两处会各自漂移，最后出现"安装包说 1.0、程序说 0.9"这种事故。
#if GetVersionNumbersString("..\bin\Release\" + MyAppExeName) != MyAppVersionNumeric
  #error 版本不一致：bin\Release\LilithReader.exe 的数字版本与 MyAppVersionNumeric 不符，请先同步 src/app/app.rc 并重新编译。
#endif

[Setup]
AppId={{A6E4B7C2-9F31-4D08-B7A5-3C8E1D2F4A90}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppComments={#MyAppDescription}
VersionInfoVersion={#MyAppVersionNumeric}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} 安装程序
VersionInfoProductName={#MyAppName}
; 数字版本字段只接受 x.x.x.x，预发布后缀（-rc.1）放到 *TextVersion 字段
VersionInfoProductVersion={#MyAppVersionNumeric}
VersionInfoTextVersion={#MyAppVersion}
VersionInfoProductTextVersion={#MyAppVersion}
DefaultDirName={autopf}\Lilith Reader
; 每用户安装：免管理员；程序目录可写（见文件头第 1 条）
PrivilegesRequired=lowest
WizardStyle=modern
DisableProgramGroupPage=yes
; 升级：沿用上次的安装目录与任务勾选，不再问一遍
UsePreviousAppDir=yes
UsePreviousTasks=yes
; 系统要求：Per-Monitor V2 DPI 与 D3D11 需要 Windows 10 1809+
MinVersion=10.0.17763
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 正在运行的程序由重启管理器处理
CloseApplications=yes
CloseApplicationsFilter=*.exe
RestartApplications=no
; 关联写入后由 Setup 统一通知外壳刷新（不需要脚本里自己调 SHChangeNotify）
ChangesAssociations=yes
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}
OutputDir=dist
OutputBaseFilename=LilithReader-{#MyAppVersion}-setup
SetupIconFile=..\assets\icon.ico
Compression=lzma2/max
SolidCompression=yes

[Languages]
Name: "chinesesimplified"; MessagesFile: "lang\ChineseSimplified.isl"

[Messages]
WelcomeLabel1=欢迎使用 [name] 安装向导
WelcomeLabel2=[name] 是一个轻量高效的文档阅读器，支持 PDF / EPUB / MOBI / FB2 / CBZ / XPS 与常见图片格式，单文件、无需额外运行库。%n%n即将在你的计算机上安装 [name] [ver]，点击"下一步"继续。

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked

[Files]
Source: "..\bin\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
; 图标放进带版本号的子目录：Explorer 图标缓存按"路径+索引"为键，路径含版本号
; 可保证每次升级后缓存自然失效，不会显示旧图标
Source: "icons\*.ico"; DestDir: "{app}\icons\{#MyAppVersion}"; Flags: ignoreversion

[InstallDelete]
; 升级前清掉旧版本的图标目录（新版本的图标在带新版本号的子目录里）
Type: filesandordirs; Name: "{app}\icons"

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[UninstallDelete]
Type: filesandordirs; Name: "{app}\icons"

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "立即运行 {#MyAppName}"; Flags: nowait postinstall skipifsilent runasoriginaluser
Filename: "ms-settings:defaultapps"; Description: "打开系统默认应用设置（把已勾选的格式设为默认打开方式）"; Flags: shellexec postinstall skipifsilent nowait runasoriginaluser unchecked

[Code]
const
  // 与 [Setup] 的 AppId 保持一致（不含首部大括号转义）
  UninstKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A6E4B7C2-9F31-4D08-B7A5-3C8E1D2F4A90}_is1';
  RootKey   = 'Software\LilithReader';

var
  // PascalScript 不支持带类型的 const 数组，用 var 数组 + InitExtArrays 初始化
  DocExts: array[0..5] of String;
  DocCaptions: array[0..5] of String;
  ImgExts: array[0..6] of String;
  AssocPage: TWizardPage;
  DocChecks: array[0..5] of TNewCheckBox;
  ImgChecks: array[0..6] of TNewCheckBox;
  PrevBindings: String;      // 上次勾选的扩展名（升级时回填）
  PrevVersion: String;       // 已安装版本（空 = 全新安装）
  PrevDir: String;           // 已安装目录
  PrevDirUsable: Boolean;    // 已安装目录是否可写（决定能否就地升级）

procedure InitExtArrays();
begin
  DocExts[0] := 'pdf';   DocCaptions[0] := '.pdf  — PDF 文档';
  DocExts[1] := 'epub';  DocCaptions[1] := '.epub — EPUB 电子书';
  DocExts[2] := 'mobi';  DocCaptions[2] := '.mobi — Kindle 电子书';
  DocExts[3] := 'fb2';   DocCaptions[3] := '.fb2  — FictionBook 电子书';
  DocExts[4] := 'cbz';   DocCaptions[4] := '.cbz  — 漫画压缩包';
  DocExts[5] := 'xps';   DocCaptions[5] := '.xps  — XPS 文档';
  ImgExts[0] := 'png';
  ImgExts[1] := 'jpg';
  ImgExts[2] := 'jpeg';
  ImgExts[3] := 'gif';
  ImgExts[4] := 'bmp';
  ImgExts[5] := 'tif';
  ImgExts[6] := 'tiff';
end;

// ---------- 升级检测 ----------

// 目录对当前用户是否可写：用"真写一个探针文件"判断，比查 ACL 可靠。
function DirWritable(const Dir: String): Boolean;
var
  Probe: String;
begin
  Probe := AddBackslash(Dir) + '~lilith-probe.tmp';
  Result := SaveStringToFile(Probe, 'probe', False);
  if Result then
    DeleteFile(Probe);
end;

// 读取已安装信息（DisplayVersion / InstallLocation）。每用户安装写 HKCU，
// 旧版曾按管理员装到 HKLM，两个根都查一遍。
function ReadPrevious(): Boolean;
begin
  PrevVersion := '';
  PrevDir := '';
  PrevDirUsable := False;
  if not RegQueryStringValue(HKCU, UninstKey, 'DisplayVersion', PrevVersion) then
    RegQueryStringValue(HKLM, UninstKey, 'DisplayVersion', PrevVersion);
  if not RegQueryStringValue(HKCU, UninstKey, 'InstallLocation', PrevDir) then
    RegQueryStringValue(HKLM, UninstKey, 'InstallLocation', PrevDir);
  if (PrevDir <> '') and DirExists(PrevDir) then
    PrevDirUsable := DirWritable(PrevDir);
  Result := PrevVersion <> '';
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  PrevBindings := '';
  ReadPrevious();
  RegQueryStringValue(HKCU, RootKey, 'Bindings', PrevBindings);
end;

// ---------- 文件关联勾选页 ----------

function HasExtInPrev(const Ext: String; Default: Boolean): Boolean;
begin
  if PrevBindings = '' then
    Result := Default
  else
    Result := (Pos(',' + Ext + ',', ',' + PrevBindings + ',') > 0);
end;

// 布局说明（为什么这样写）：
// Inno 的 TNewCheckBox 不支持 AutoSize，长文字放里面必然面临"固定宽度 vs 文字
// 随字体/缩放变化"的截断问题。因此：
//   - 勾选框只放短文字（如 ".pdf"），固定宽度留数倍余量（宽度与文字都随 DPI
//     线性缩放，余量比例恒定，不会截断）；
//   - 格式描述放旁边的 TNewStaticText，其 AutoSize 由 VCL 按当前字体实测，
//     框架保证完整显示；
//   - 描述同时写入 Hint，悬停可见。
// 不引入任何外部 DLL 调用。

function MakeLabel(const Text: String; X, Y: Integer): TNewStaticText;
var
  Box: TNewStaticText;
begin
  Box := TNewStaticText.Create(AssocPage);
  Box.Parent := AssocPage.Surface;
  Box.Caption := Text;
  Box.Left := ScaleX(X);
  Box.Top := ScaleY(Y);
  Box.AutoSize := True;
  Result := Box;
end;

procedure MakeCheck(var Box: TNewCheckBox; const Ext, Hint: String;
  X, Y: Integer; Checked: Boolean);
begin
  Box := TNewCheckBox.Create(AssocPage);
  Box.Parent := AssocPage.Surface;
  Box.Caption := '.' + Ext;
  Box.Left := ScaleX(X);
  Box.Top := ScaleY(Y);
  Box.Width := ScaleX(72);
  // 默认高度按 96 DPI 硬编码，字体随 DPI 放大后会被裁顶、挤压；显式随 ScaleY 缩放
  Box.Height := ScaleY(24);
  Box.Hint := Hint;
  Box.ShowHint := True;
  Box.Checked := Checked;
end;

procedure CreateAssocPage();
var
  I, RowY: Integer;
  Note: TNewStaticText;
begin
  AssocPage := CreateCustomPage(wpSelectTasks, '文件关联',
    '选择希望由 Lilith Reader 直接打开的文件类型；以后随时可在系统"默认应用"设置中更改');

  MakeLabel('文档格式', 12, 4);
  for I := 0 to 5 do
  begin
    RowY := 30 + I * 22;
    MakeCheck(DocChecks[I], DocExts[I], DocCaptions[I], 28, RowY,
      HasExtInPrev(DocExts[I], True));
    MakeLabel(DocCaptions[I], 108, RowY + 2);
  end;

  // 右列：图片格式（默认不勾——当前按"单页文档"打开，尚非专门看图器，
  // 避免改变用户已有的看图习惯；将来做图片浏览器功能后可默认放开）
  MakeLabel('图片格式', 288, 4);
  for I := 0 to 6 do
  begin
    RowY := 30 + I * 22;
    MakeCheck(ImgChecks[I], ImgExts[I], '', 288, RowY,
      HasExtInPrev(ImgExts[I], False));
  end;

  // 底部说明：宽度跟随面板（不写死像素），按当前 DPI 自动换行
  Note := MakeLabel('图片通常已由看图软件打开，建议保持不勾选；勾选后双击图片将改用 Lilith Reader。',
    12, 188);
  Note.AutoSize := False;
  Note.WordWrap := True;
  Note.Width := AssocPage.Surface.Width - ScaleX(24);
  Note.Height := ScaleY(32);
end;

procedure InitializeWizard();
var
  NewDir: String;
begin
  InitExtArrays();
  CreateAssocPage();

  // 欢迎页：升级时把"从哪个版本升到哪个版本、数据保留"说清楚
  if PrevVersion <> '' then
    WizardForm.WelcomeLabel2.Caption :=
      '检测到已安装 {#MyAppName} ' + PrevVersion + '，本次将升级到 {#MyAppVersion}。' + #13#10 + #13#10 +
      '这是原地覆盖升级：安装位置与你的阅读数据（阅读位置、书签、界面偏好）都会保留。' + #13#10 + #13#10 +
      '点击"下一步"继续。';

  // 自动定位到上次的安装目录。旧版本若装在 Program Files（需要管理员），
  // 而本版本是每用户安装，就回落到每用户目录并说明原因。
  if PrevDir <> '' then
  begin
    if PrevDirUsable then
      WizardForm.DirEdit.Text := PrevDir
    else
    begin
      NewDir := ExpandConstant('{autopf}\Lilith Reader');
      WizardForm.DirEdit.Text := NewDir;
      MsgBox('检测到旧版本安装在：' + #13#10 + PrevDir + #13#10 + #13#10 +
             '该目录需要管理员权限，而本版本按"仅为当前用户"安装（无需管理员；' + #13#10 +
             '设置与阅读进度需要写在程序目录里，装在 Program Files 下会写不进去）。' + #13#10 + #13#10 +
             '本次将安装到：' + #13#10 + NewDir + #13#10 + #13#10 +
             '旧目录不会自动删除；可在"设置 → 应用"里卸载旧版本后再手动删除。',
             mbInformation, MB_OK);
    end;
  end;
end;

// ---------- 关联注册表写入（全部用户级 HKCU） ----------

procedure BindOneExt(const Ext: String);
var
  ProgId, Cmd, Icon, AppPath: String;
begin
  AppPath := ExpandConstant('{app}\{#MyAppExeName}');
  ProgId := 'LilithReader.' + Ext;
  Icon := ExpandConstant('{app}\icons\{#MyAppVersion}') + '\lilith_' + Uppercase(Ext) + '.ico';
  Cmd := '"' + AppPath + '" "%1"';

  // ProgId 本体
  RegWriteStringValue(HKCU, 'Software\Classes\' + ProgId, '',
    'Lilith Reader ' + Uppercase(Ext) + ' 文档');
  RegWriteStringValue(HKCU, 'Software\Classes\' + ProgId, 'FriendlyTypeName',
    'Lilith Reader ' + Uppercase(Ext) + ' 文档');
  RegWriteStringValue(HKCU, 'Software\Classes\' + ProgId + '\DefaultIcon', '', Icon);
  RegWriteStringValue(HKCU, 'Software\Classes\' + ProgId + '\shell\open\command', '', Cmd);

  // 挂进该扩展名的"打开方式"候选列表（即使不设默认，右键也能直达）
  RegWriteStringValue(HKCU, 'Software\Classes\.' + Ext + '\OpenWithProgids', ProgId, '');

  // Capabilities：让系统"默认应用"页认识我们（Win11 可一键设默认）
  RegWriteStringValue(HKCU, RootKey + '\Capabilities\FileAssociations',
    '.' + Ext, ProgId);
end;

procedure WriteAssociations();
var
  I: Integer;
  Bindings, AppPath: String;
begin
  AppPath := ExpandConstant('{app}\{#MyAppExeName}');
  Bindings := '';

  RegWriteStringValue(HKCU, RootKey + '\Capabilities', 'ApplicationName',
    ExpandConstant('{#MyAppName}'));
  RegWriteStringValue(HKCU, RootKey + '\Capabilities', 'ApplicationDescription',
    ExpandConstant('{#MyAppDescription}'));
  RegWriteStringValue(HKCU, RootKey + '\Capabilities', 'ApplicationIcon', AppPath);

  for I := 0 to 5 do
    if DocChecks[I].Checked then
    begin
      BindOneExt(DocExts[I]);
      if Bindings <> '' then Bindings := Bindings + ',';
      Bindings := Bindings + DocExts[I];
    end;
  for I := 0 to 6 do
    if ImgChecks[I].Checked then
    begin
      BindOneExt(ImgExts[I]);
      if Bindings <> '' then Bindings := Bindings + ',';
      Bindings := Bindings + ImgExts[I];
    end;

  // 记录本次实际绑定的扩展名，卸载时精确清理
  RegWriteStringValue(HKCU, RootKey, 'Bindings', Bindings);

  // 登记到系统"默认应用"候选（一个都没勾时也登记，Capabilities 为空无害）
  RegWriteStringValue(HKCU, 'Software\RegisteredApplications',
    ExpandConstant('{#MyAppName}'), RootKey + '\Capabilities');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    WriteAssociations();
end;

// ---------- 卸载清理 ----------

// Bindings 是逗号分隔串；返回第 Index 个（0 起）token，越界返回空串。
// Inno 脚本对数组传参支持有限，用按索引取值代替 split。
function BindingAt(const Bindings: String; Index: Integer): String;
var
  I, Start, Found: Integer;
begin
  Result := '';
  Start := 1;
  Found := 0;
  for I := 1 to Length(Bindings) + 1 do
  begin
    if (I > Length(Bindings)) or (Bindings[I] = ',') then
    begin
      if Found = Index then
      begin
        Result := Trim(Copy(Bindings, Start, I - Start));
        Exit;
      end;
      Found := Found + 1;
      Start := I + 1;
    end;
  end;
end;

// 程序目录里属于"用户数据"的文件（卸载时按需保留/删除）
function DataFiles(): TArrayOfString;
var
  N: Integer;
  Names: array[0..1] of String;
  I: Integer;
  P: String;
begin
  SetArrayLength(Result, 0);
  Names[0] := 'LilithReader.ini';
  Names[1] := 'reader_state.bin';
  N := 0;
  for I := 0 to 1 do
  begin
    P := ExpandConstant('{app}\' + Names[I]);
    if FileExists(P) then
    begin
      SetArrayLength(Result, N + 1);
      Result[N] := P;
      N := N + 1;
    end;
  end;
end;

procedure RemoveAssociations();
var
  I: Integer;
  Ext, ProgId, Bindings: String;
begin
  Bindings := '';
  RegQueryStringValue(HKCU, RootKey, 'Bindings', Bindings);

  // 只清理我们写入的 ProgId 与 OpenWithProgids 值，不碰用户当前的默认选择
  if Bindings <> '' then
  begin
    I := 0;
    repeat
      Ext := BindingAt(Bindings, I);
      if Ext <> '' then
      begin
        ProgId := 'LilithReader.' + Ext;
        RegDeleteKeyIncludingSubkeys(HKCU, 'Software\Classes\' + ProgId);
        if RegValueExists(HKCU, 'Software\Classes\.' + Ext + '\OpenWithProgids', ProgId) then
          RegDeleteValue(HKCU, 'Software\Classes\.' + Ext + '\OpenWithProgids', ProgId);
      end;
      I := I + 1;
    until (Ext = '') or (I > 13);
  end;

  RegDeleteKeyIncludingSubkeys(HKCU, RootKey + '\Capabilities');
  if RegValueExists(HKCU, 'Software\RegisteredApplications',
     ExpandConstant('{#MyAppName}')) then
    RegDeleteValue(HKCU, 'Software\RegisteredApplications',
      ExpandConstant('{#MyAppName}'));
  RegDeleteKeyIncludingSubkeys(HKCU, RootKey);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  I: Integer;
  Files: TArrayOfString;
  Labels: TArrayOfString;
begin
  if CurUninstallStep = usUninstall then
  begin
    // 只在数据确实存在、且非静默卸载时问一次；默认"保留"（静默卸载一律保留，绝不误删）
    Files := DataFiles();
    if (GetArrayLength(Files) > 0) and (not UninstallSilent()) then
    begin
      SetArrayLength(Labels, 2);
      Labels[0] := '保留阅读数据（推荐）';
      Labels[1] := '一并删除';
      if TaskDialogMsgBox(
           '是否保留阅读数据？',
           '你的设置与阅读进度保存在程序目录中：' + #13#10 +
           '   · LilithReader.ini —— 界面与阅读偏好' + #13#10 +
           '   · reader_state.bin —— 每本书的阅读位置与书签' + #13#10 + #13#10 +
           '保留后，以后重新安装可以接着上次的位置继续阅读。',
           mbConfirmation, MB_YESNO, Labels, 0) = IDNO then
        for I := 0 to GetArrayLength(Files) - 1 do
          DeleteFile(Files[I]);
    end;
    Exit;
  end;

  if CurUninstallStep = usPostUninstall then
    RemoveAssociations();
end;
