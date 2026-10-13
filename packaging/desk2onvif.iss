; Desk2ONVIF — Inno Setup 安装脚本（简体中文向导）
;
; 编译：ISCC.exe packaging\desk2onvif.iss
;      或 scripts\package.ps1（会先把后端和前端都构建好）
;
; 版本号从命令行传进来（/DMyAppVersion=0.1.0），由 package.ps1 从
; CMakeLists.txt 里读。**不在这里再写一份** —— 两处版本号迟早不一致，
; 而症状是"安装程序显示的版本和程序 --version 输出的不是一个"。

#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#define MyAppName "Desk2ONVIF"
#define MyAppPublisher "Desk2ONVIF"
#define MyAppExeName "desk2onvif.exe"

; 项目主页。**故意没有默认值** —— 之前这里写的是 "https://github.com/"，那是个
; 占位符，装出来的程序"帮助/支持"里点开是 GitHub 首页，指向一个跟本项目毫无
; 关系的页面，比没有链接更糟。有真地址了再从外面传：
;
;     ISCC.exe /DMyAppUrl=https://github.com/<你>/Desk2ONVIF packaging\desk2onvif.iss
;     （scripts\package.ps1 的 -AppUrl 参数就是干这个的）
;
; 没传就不写 AppPublisherURL 这一项，安装程序里也就不显示那条链接。
#ifndef MyAppUrl
  #define MyAppUrl ""
#endif

[Setup]
; AppId 唯一标识这个应用。**升级安装靠它识别**，所以它一经发布就不能再改：
; 改了以后新版本会装成"另一个程序"，旧版不会被卸载，开始菜单里出现两个。
; 这个 GUID 是给 Desk2ONVIF 生成的，不要复用 fsrec 那一个。
AppId={{7D3F1A62-5C48-4E9B-8A17-6F20D9B4E531}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
; 没传 MyAppUrl 时这一项**整行不出现**。写成 AppPublisherURL={#MyAppUrl} 让它是
; 空串的话，安装程序里会出现一个点开没反应的链接。
; （`#if` 里的字符串比较实测可用 —— 见 scripts/package.ps1 的 -AppUrl。）
#if MyAppUrl != ""
AppPublisherURL={#MyAppUrl}
#endif

; PrivilegesRequired=lowest + {autopf} ⇒ 装到 %LOCALAPPDATA%\Programs\Desk2ONVIF。
; 这是**刻意**的选择，不是省事：
;   1. 装的时候不弹 UAC，用完卸载也不留残留；
;   2. 那个目录**当前用户可写** —— 程序要把 config.json 写在 exe 旁边
;      （绿色版语义，删掉就是恢复默认值）。装到 Program Files 的话，
;      写配置每次都要管理员权限，而失败的表现是"改了配置存不下来"，很难查；
;   3. 这个程序必须由**登录用户**在他的桌面会话里运行才抓得到画面。
;      装成服务或在 Session 0 跑，DXGI 根本抓不到用户桌面 —— 和核心需求相冲。
PrivilegesRequired=lowest
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\release
OutputBaseFilename={#MyAppName}_Setup_{#MyAppVersion}
SetupIconFile=..\resources\desk2onvif.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 程序是 MinGW 构建的，不带 MSVC 运行库依赖，所以不需要 vcredist。
; （fsrec 要，因为它用 MSVC 编译。这里不能照抄那一段。）

[Languages]
Name: "chinesesimplified"; MessagesFile: "languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："

[Files]
Source: "..\build\bin\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
; DLL 用通配符而不是列 16 个名字：换 FFmpeg 版本、多一个依赖，
; 列名字的写法会安静地漏掉新文件，而症状是"装完启动失败、缺 DLL"。
Source: "..\build\bin\*.dll"; DestDir: "{app}"; Flags: ignoreversion
; 前端产物。**这一条不能少** —— 少了它程序照跑、推流照推、ONVIF 照应答，
; 只有网页界面打不开，而且一个字都不报（exe 的 --web-root 默认值是相对路径
; web/dist，"目录不存在"被刻意当成"没挂静态资源"而不是错误）。
Source: "..\web\dist\*"; DestDir: "{app}\web\dist"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\packaging\THIRD-PARTY.md"; DestDir: "{app}"; Flags: ignoreversion
; 改名要用 DestName，不是 DestDir。写成 DestDir={app}\README.md 的后果是
; Inno 建了一个**叫 README.md 的目录**，把 README-release.md 塞进去 ——
; 装完看着像成功，[Icons] 里那条"打开使用说明"点开却是个文件夹。
Source: "..\packaging\README-release.md"; DestDir: "{app}"; DestName: "README.md"; Flags: ignoreversion
Source: "..\packaging\start.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\packaging\start.bat"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\packaging\start-local.bat"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\packaging\list-displays.bat"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\packaging\selfcheck.bat"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\packaging\firewall-allow.bat"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\packaging\firewall-print.bat"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\resources\desk2onvif.ico"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
; 主入口走 start.bat 而不是 exe：批处理会先把端口/监听地址/凭据拼好再启动，
; 直接点 exe 的话用的是内置默认值，装的时候选的那些设置就白选了。
Name: "{group}\启动 {#MyAppName}"; Filename: "{app}\start.bat"; WorkingDir: "{app}"; IconFilename: "{app}\desk2onvif.ico"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\start.bat"; Tasks: desktopicon; WorkingDir: "{app}"; IconFilename: "{app}\desk2onvif.ico"

[Run]
; 装完问一句要不要立刻启动。skipifsilent 是给无人值守安装用的。
Filename: "{app}\start.bat"; Description: "立即启动 {#MyAppName}"; Flags: postinstall nowait skipifsilent
Filename: "{app}\README.md"; Description: "打开使用说明"; Flags: postinstall shellexec skipifsilent unchecked
Filename: "{app}\list-displays.bat"; Description: "先看看这台机器能不能录（推荐第一次运行前跑）"; Flags: postinstall nowait skipifsilent unchecked

[UninstallDelete]
; config.json 是 [Code] 在 ssPostInstall 里写的，**不在** Inno 的安装清单里
; （unins000.dat 只记 [Files] 装下去的那些）。什么都不写的话，静默卸载会把
; 整个目录删干净、只留一个 config.json，于是 {app} 目录不空、不会被移除 ——
; 表现为"卸载完了还剩一个目录，里面一份配置"。
; 交互式卸载里选"保留配置"的那条路会先把它抄到 {userappdata}，所以这里
; 无条件删是安全的：要留的已经留过了。usUninstall 阶段跑在这之前。
Type: files; Name: "{app}\config.json"
Type: files; Name: "{app}\config.json.bak"

; 上面那条判据漏了一整类：**运行期才出现的文件**。【2026-10-08 实测】
; 操作日志（第四轮 Phase V）落在 {app}\logs\audit-YYYY-MM-DD.log —— 同样是运行时
; 才建的，不在 unins000.dat 里。不声明的话，静默卸载把 config.json 删掉、
; 却把 logs\ 整个留下，{app} 于是照样不空、照样不被移除：
;   [FAIL] 安装目录还在（C:\...\Desk2ONVIF-verify）
;   [FAIL] 卸载没有留下多余东西（剩 2 项）
;          logs, logs\audit-2026-10-08.log
; 症状与 config.json 那一版一模一样，只是文件多了一层目录。绿色版验收没有这一条
; （它靠 harness 删临时目录），所以只有安装包这一路能照出来。
; filesandordirs 而不是 files：目录本身也得删掉，否则 {app}\logs 空着一样让
; {app} 不空。范围限死在 {app} 里面，不递归到别处。
Type: filesandordirs; Name: "{app}\logs"
; 同一类：控制台形态（--console）会写 <配置目录>\desk2onvif.log 及其轮转的 .old。
; 这一次的验收没跑到它们（验收里起的是裸形态 --http，那条路按设计不写日志文件），
; 所以这两条**不是实测出来的**，是按上面同一个机制补上的 —— 写在这里以免
; 下次有人以为是漏了。它们同样只落在 {app} 里。
Type: files; Name: "{app}\desk2onvif.log"
Type: files; Name: "{app}\desk2onvif.log.old"

[Code]
var
  PortsPage: TInputQueryWizardPage;
  ScopePage: TInputOptionWizardPage;
  CredPage:  TInputQueryWizardPage;
  SelectedRtspPort: Integer;
  SelectedHttpPort: Integer;
  SelectedHost:     String;
  SelectedUser:     String;
  SelectedPass:     String;

{ 端口是否已被占用。用 PowerShell 试连一次 —— 比在 Pascal 里手搓
  winsock 短得多，而且失败时不会把安装程序带崩。 }
function PortInUse(Port: Integer): Boolean;
var
  ResultCode: Integer;
  PsFile, OutFile, Script: String;
  Line: AnsiString;
begin
  Result := False;
  PsFile  := ExpandConstant('{tmp}\d2o_portcheck.ps1');
  OutFile := ExpandConstant('{tmp}\d2o_portcheck.txt');
  Script :=
    '$p = ' + IntToStr(Port) + #13#10 +
    'try {' + #13#10 +
    '  $c = New-Object System.Net.Sockets.TcpClient' + #13#10 +
    '  $c.Connect("127.0.0.1", $p)' + #13#10 +
    '  $c.Close()' + #13#10 +
    '  "INUSE"' + #13#10 +
    '} catch {' + #13#10 +
    '  "FREE"' + #13#10 +
    '}';
  SaveStringToFile(PsFile, Script, False);
  Exec(ExpandConstant('{cmd}'),
       '/C powershell.exe -NoProfile -ExecutionPolicy Bypass -File "' + PsFile + '" > "' + OutFile + '"',
       '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  if LoadStringFromFile(OutFile, Line) then
    Result := (Pos('INUSE', Line) > 0);
end;

{ JSON 字符串转义。**必须要**：口令里一个反斜杠或引号就会写出语法非法的
  config.json，而程序启动时读配置失败是**直接拒绝启动**的 —— 用户看到的是
  "闪一下就没了"，很难想到是自己口令里的一个引号。 }
function JsonEscape(const S: String): String;
var
  I: Integer;
  C: Char;
begin
  Result := '';
  for I := 1 to Length(S) do
  begin
    C := S[I];
    if      C = '"'  then Result := Result + '\"'
    else if C = '\'  then Result := Result + '\\'
    else if C = #8   then Result := Result + '\b'
    else if C = #9   then Result := Result + '\t'
    else if C = #10  then Result := Result + '\n'
    else if C = #12  then Result := Result + '\f'
    else if C = #13  then Result := Result + '\r'
    { Inno 的 Pascal Script 里**没有** IntToHex（有 Format），所以走 Format。 }
    else if Ord(C) < 32 then Result := Result + '\u' + Format('%.4x', [Ord(C)])
    else Result := Result + C;
  end;
end;

{ 写配置文件。字段与 src/common/config.cpp 的 config_from_json 一一对应；
  没写进去的字段会落到程序内置默认值，所以这里只写"安装时问过的"那几个，
  其余留空反而更稳（将来加字段不用改安装程序）。

  **`web` 对象里必须带上 user / password —— 这一条是真出过事的。**
  这里原来只写 enabled/host/port，于是向导里让用户填的那一对只落到了 ONVIF 上；
  Web 控制台拿不到它，只能落回内置默认值 admin/admin。用户的体验是
  "我在安装时明明设了口令，网页却怎么都登不进去，只有 admin/admin 能进"，
  而日志、安装日志、程序输出里**没有一个字**提到这件事。
  现在两处写同一对（同一个向导页、同一份 SelectedUser/SelectedPass），
  装完之后它们仍是**各自独立**的字段：在控制台改网页口令不会动到 NVR 那份。

  **编码**：用 SaveStringsToUTF8FileWithoutBOM，不用 SaveStringToFile。
  后者对不含 BOM 的字符串按**系统 ANSI 代码页**（这里是 936/GBK）落盘，
  而程序那边是 std::ifstream 读字节 + nlohmann::json::parse —— 它按 UTF-8 解。
  于是"口令里有一个中文"会写成 GBK 字节、被当成乱码 UTF-8 读，
  结果是**用户按自己填的口令登不进去**，症状与上面那条一模一样。
  BOM 也不能加：nlohmann 的 parse 遇到 EF BB BF 会直接抛
  "invalid literal"，而读配置失败是**拒绝启动**的。UTF-8 无 BOM 两边都对。 }
procedure WriteConfig;
var
  Json, Path, Bak: String;
  Lines: TArrayOfString;
begin
  Path := ExpandConstant('{app}\config.json');

  { 重装/升级时会**覆盖**已有的 config.json，而向导里的端口默认是 554/8080 ——
     用户上次填的可能是别的，一路点"下一步"就把自己的配置洗掉了。
     与其去解析旧配置再回填向导（解析错了更糟），不如先把旧的那份留个 .bak：
     不猜、不丢，代价是目录里多一个文件。 }
  if FileExists(Path) then
  begin
    Bak := Path + '.bak';
    if FileExists(Bak) then DeleteFile(Bak);
    if CopyFile(Path, Bak, False) then
      Log('原有 config.json 已备份为 config.json.bak');
  end;

  Json :=
    '{' + #13#10 +
    '  "rtsp":  { "enabled": true, "port": ' + IntToStr(SelectedRtspPort) + ', "path": "/live",' + #13#10 +
    '             "sub_enabled": true, "sub_path": "/live_sub",' + #13#10 +
    '             "sub_height": 360, "sub_bitrate_kbps": 512 },' + #13#10 +
    '  "web":   { "enabled": true, "host": "' + JsonEscape(SelectedHost) + '", "port": ' + IntToStr(SelectedHttpPort) + ',' + #13#10 +
    '             "auth_enabled": true, "user": "' + JsonEscape(SelectedUser) + '",' + #13#10 +
    '             "password": "' + JsonEscape(SelectedPass) + '" },' + #13#10 +
    '  "onvif": { "enabled": true, "user": "' + JsonEscape(SelectedUser) + '",' + #13#10 +
    '             "password": "' + JsonEscape(SelectedPass) + '",' + #13#10 +
    '             "manufacturer": "Desk2ONVIF", "model": "Desk2ONVIF Virtual Camera",' + #13#10 +
    '             "firmware_version": "' + '{#MyAppVersion}' + '", "hardware_id": "1.0",' + #13#10 +
    '             "serial_number": "", "device_name": "Desk2ONVIF", "location": "Desktop" }' + #13#10 +
    '}' + #13#10;
  { UTF-8 **无 BOM**，理由见上面那段注释。签名已从编译器自己的声明表里核对过：
    function SaveStringsToUTF8FileWithoutBOM(const FileName: String;
      const S: TArrayOfString; const Append: Boolean): Boolean; }
  SetArrayLength(Lines, 1);
  Lines[0] := Json;
  if not SaveStringsToUTF8FileWithoutBOM(Path, Lines, False) then
  begin
    { 静默安装下这条路只有一个出口：日志。弹框就是挂住。 }
    if WizardSilent() then
      Log('错误：写不了配置文件 ' + Path + '，程序将使用内置默认值。')
    else
      MsgBox('写不了配置文件：' + Path + #13#10 +
             '程序仍能启动，但会使用内置默认值。', mbError, MB_OK);
  end;
end;

{ 监听地址校验。允许 IPv4 字面量或 0.0.0.0；不做域名解析 —— 那一层由
  libuv 在 bind 时处理，报的错比这里猜的准。 }
function IsValidHost(const S: String): Boolean;
var
  I, Num, Part, Dots: Integer;
  Ch: Char;
begin
  Result := False;
  if Trim(S) = '' then Exit;
  Num := 0; Part := 0; Dots := 0;
  for I := 1 to Length(S) do
  begin
    Ch := S[I];
    if Ch = '.' then
    begin
      if (Part = 0) or (Num > 255) then Exit;
      Inc(Dots); Num := 0; Part := 0;
    end
    else if (Ch >= '0') and (Ch <= '9') then
    begin
      Num := Num * 10 + (Ord(Ch) - Ord('0'));
      if Num > 255 then Exit;
      Inc(Part);
    end
    else Exit;
  end;
  if (Part = 0) or (Num > 255) or (Dots <> 3) then Exit;
  Result := True;
end;

{ 读一个自定义命令行参数（/D2OXXX=值），没有就用默认值。
  Inno 允许任何未知的 /NAME=VALUE 通过 ExpandConstant 的 param 常量取出来，
  不用注册；前缀 D2O 是为了不和 Inno 自己的开关（/DIR、/LOG、/USERNAME…）撞名。
  （顺带一条：Pascal 的注释不嵌套 —— 像上面这种说明里**不能**出现右花括号，
  出现一个就当场把注释结束了，报的是"注释后面那几行语法错误"，指不到这里。） }
function GetParam(const Name, Default: String): String;
begin
  Result := Trim(ExpandConstant('{param:' + Name + '|' + Default + '}'));
end;

{ 读一个环境变量，没定义就给空串。
  Inno 对没定义的变量是"原样留着"还是"给空串"在文档里不够明确，两种都兜住：
  展开结果里还剩占位符本身，就当没给。 }
function EnvValue(const Name: String): String;
begin
  Result := Trim(ExpandConstant('{%' + Name + '}'));
  if Pos('{%', Result) > 0 then Result := '';
end;

{ 口令为什么要有环境变量这条路：命令行 /D2OPASS= 走的是 Inno 自己的开关
  解析，它**不按** MSVC 那套"反斜杠转义引号"的规则来 —— 值里出现一个双引号
  会被当成引号分隔符吃掉，传进来的口令就不是原样了。而口令里有引号或反斜杠
  是很正常的事，转义一旦没做对，写出来的 config.json 就是语法非法的，
  程序会直接拒绝启动，用户看到的是"闪一下就没了"。
  环境变量不过这一层解析，从根上绕开。
  优先级：命令行参数 > 环境变量 > 内置默认值。 }
function GetParamPort(const Name: String; Default: Integer): Integer;
begin
  Result := StrToIntDef(GetParam(Name, ''), Default);
  if (Result < 1) or (Result > 65535) then
  begin
    { 参数写错时**不能弹框**：无人值守安装里没人看得到它。回落到默认值并把
       这件事写进日志 —— 静默安装的那份日志是唯一能看到这句话的地方。 }
    Log('警告：参数 ' + Name + ' 的值不合法，改用 ' + IntToStr(Default) + '。');
    Result := Default;
  end;
end;

procedure InitializeWizard();
var
  Scope: String;
begin
  { 命令行参数先取，再拿它们做向导页的初值。这样同一条命令既能用来无人值守
     安装，也能给交互式安装预填一份合适的值（/D2OUSER=xxx 之类）。 }
  SelectedRtspPort := GetParamPort('D2ORTSPORT', 554);
  SelectedHttpPort := GetParamPort('D2OHTTPPORT', 8080);
  if SelectedRtspPort = SelectedHttpPort then
  begin
    Log('警告：D2ORTSPORT 和 D2OHTTPPORT 不能相同，Web 端口改用 8080。');
    SelectedHttpPort := 8080;
    if SelectedRtspPort = SelectedHttpPort then SelectedHttpPort := 8081;
  end;

  Scope := LowerCase(GetParam('D2OSCOPE', 'all'));
  if Scope = 'local' then SelectedHost := '127.0.0.1'
  else if (Scope = 'all') or (Scope = 'lan') then SelectedHost := '0.0.0.0'
  else
  begin
    Log('警告：D2OSCOPE 只能是 all 或 local，改用 all。');
    SelectedHost := '0.0.0.0';
  end;

  SelectedUser := GetParam('D2OUSER', '');
  if SelectedUser = '' then SelectedUser := EnvValue('DESK2ONVIF_USER');
  if SelectedUser = '' then
  begin
    SelectedUser := 'admin';
    Log('未指定登录用户名，用默认值 admin（Web 控制台与 ONVIF 共用这一份）。');
  end;

  SelectedPass := GetParam('D2OPASS', '');
  if SelectedPass = '' then SelectedPass := EnvValue('DESK2ONVIF_PASSWORD');
  if SelectedPass = '' then
  begin
    SelectedPass := 'admin';
    Log('未指定登录口令，用默认值 admin。口令是公开的出厂值，建议改掉。');
  end;
  { 口令**不进日志**。安装日志经常被贴进工单和聊天窗口。 }

  { 只记用户名，不记口令 —— 这一行会连同上面那两条一起去查"安装时到底
     用了什么账号"，而它记的正是会被写进 config.json 的 web.user / onvif.user。 }
  Log('安装参数：RTSP 端口 ' + IntToStr(SelectedRtspPort) +
      '，Web 端口 ' + IntToStr(SelectedHttpPort) +
      '，监听 ' + SelectedHost +
      '，登录用户 ' + SelectedUser + '（Web 与 ONVIF 共用）');

  { 先把话说清楚再让人选监听范围。Web 控制台**是要登录的**，但出厂口令是
     公开的 admin/admin —— "没改过口令"和"没鉴权"在效果上几乎一样，而它默认
     就要监听 0.0.0.0。选之前得知道这意味着什么。 }
  ScopePage := CreateInputOptionPage(wpLicense,
    '监听范围',
    '谁可以访问 Web 控制台和 RTSP',
    'Desk2ONVIF 的 Web 控制台（预览页、状态页、配置页）需要登录，' + #13#10 +
    '但**出厂用户名/口令是 admin/admin，是公开的**，装完请立刻改掉。' + #13#10 +
    '在改掉之前，如果监听所有网卡，同一局域网内任何一台机器都能进来看你的桌面画面、读配置、改配置。' + #13#10 + #13#10 +
    '下一页让你填的那一份用户名/口令，**Web 登录和 NVR 用的都是它**（装完之后' + #13#10 +
    '两者在控制台里仍是各自独立的字段，改网页口令不会动到 NVR 那份）。' + #13#10 + #13#10 +
    '要让 NVR 或别的机器拉流，必须选"监听所有网卡"；只想本机自己看就选"仅本机"。' + #13#10 +
    '装完也可以改：运行 start-local.bat 即可切到仅本机。',
    True, False);
  ScopePage.Add('监听所有网卡（0.0.0.0）—— NVR 和局域网其他设备可访问');
  ScopePage.Add('仅本机（127.0.0.1）—— 只有这台机器能访问，最安全');
  ScopePage.Values[0] := (SelectedHost <> '127.0.0.1');
  ScopePage.Values[1] := (SelectedHost = '127.0.0.1');

  PortsPage := CreateInputQueryPage(ScopePage.ID,
    '端口设置',
    'RTSP 与 Web 控制台用哪两个端口',
    'RTSP 端口是 NVR 拉流用的，一般保持 554（NVR 的默认值就是它）。' + #13#10 +
    'Web 端口供浏览器打开配置界面，NVR 添加 ONVIF 通道时也会用到。' + #13#10 + #13#10 +
    '另外还会用到 UDP 3702（WS-Discovery 组播），NVR 靠它自动发现设备 —— ' + #13#10 +
    '这个端口是固定的，无法配置。');
  PortsPage.Add('RTSP 端口：', False);
  PortsPage.Add('Web 端口：', False);
  PortsPage.Values[0] := IntToStr(SelectedRtspPort);
  PortsPage.Values[1] := IntToStr(SelectedHttpPort);

  CredPage := CreateInputQueryPage(PortsPage.ID,
    '登录凭据',
    'Web 控制台与 NVR 都用这一份用户名和口令',
    '这一对会同时写进配置文件的 web 与 onvif 两处：' + #13#10 +
    '  · 打开 Web 控制台（预览页/状态页/配置页）时登录用的就是它；' + #13#10 +
    '  · 在 NVR 上添加这台"摄像机"时填的摄像机用户名/密码，也是它。' + #13#10 + #13#10 +
    '**请改掉默认口令**：出厂值 admin/admin 是公开的。' + #13#10 +
    '注意口令留空在这程序里**不等于关闭鉴权**，而是"口令就是空字符串"，' + #13#10 +
    '任何知道用户名的人都能进 —— 所以不要留空。' + #13#10 + #13#10 +
    '装完之后这两份是**各自独立**的字段：在控制台里改网页口令不会连带改掉 NVR 那份，' + #13#10 +
    '反之亦然（这样改网页口令就不用把 NVR 全部重配一遍）。');
  CredPage.Add('用户名：', False);
  CredPage.Add('口令：', True);          { True = 输入时显示为星号 }
  CredPage.Values[0] := SelectedUser;
  CredPage.Values[1] := SelectedPass;
end;

{ 翻页前的处理。这里有个**必须记住的坑**：MsgBox 在 /VERYSILENT 下不会被
  /SUPPRESSMSGBOXES 自动应答 —— 它会一直等一个永远不来的点击，安装就挂
  在那里：没有报错、没有退出码（进程还没退）、日志最后一行是个问句、
  目标目录根本没建。这个症状和"安装程序坏了"长得一模一样，踩过一次。
  所以凡"确认"性质的 MsgBox 一律套 WizardSilent()；静默安装下这些问题的
  答案由命令行参数决定（见 InitializeWizard）。 }
function NextButtonClick(CurPageID: Integer): Boolean;
var
  RtspPort, HttpPort: Integer;
begin
  Result := True;

  if CurPageID = ScopePage.ID then
  begin
    if ScopePage.Values[1] then SelectedHost := '127.0.0.1'
    else SelectedHost := '0.0.0.0';
  end
  else if CurPageID = PortsPage.ID then
  begin
    RtspPort := StrToIntDef(Trim(PortsPage.Values[0]), 0);
    HttpPort := StrToIntDef(Trim(PortsPage.Values[1]), 0);

    { 端口是**现在**检查，不是启动时。装完才发现端口被占，用户已经点完
       "完成"了，得自己回头猜是哪一步出的问题。
       静默安装下没人可问，也不能因为一个警告就中止 —— 写进日志，
       让布署的人自己去查（那份额外的 /LOG 是唯一能看到它的地方）。 }
    if WizardSilent() then
    begin
      if PortInUse(RtspPort) then
        Log('警告：RTSP 端口 ' + IntToStr(RtspPort) + ' 已被占用，程序启动时会 bind 失败。');
      if PortInUse(HttpPort) then
        Log('警告：Web 端口 ' + IntToStr(HttpPort) + ' 已被占用，程序启动时会 bind 失败。');
      SelectedRtspPort := RtspPort;
      SelectedHttpPort := HttpPort;
      Exit;
    end;

    if (RtspPort < 1) or (RtspPort > 65535) then
    begin
      MsgBox('RTSP 端口必须是 1–65535 之间的整数。', mbError, MB_OK);
      Result := False; Exit;
    end;
    if (HttpPort < 1) or (HttpPort > 65535) then
    begin
      MsgBox('Web 端口必须是 1–65535 之间的整数。', mbError, MB_OK);
      Result := False; Exit;
    end;
    if RtspPort = HttpPort then
    begin
      MsgBox('RTSP 端口和 Web 端口不能是同一个 —— 两个服务抢同一个端口，' + #13#10 +
             '先起来的那个赢，另一个直接 bind 失败。', mbError, MB_OK);
      Result := False; Exit;
    end;
    if PortInUse(RtspPort) then
    begin
      MsgBox('RTSP 端口 ' + IntToStr(RtspPort) + ' 已经被占用，请换一个。' + #13#10 + #13#10 +
             '（如果这台机器上已经有别的 RTSP 服务，比如另一个 Desk2ONVIF，' + #13#10 +
             '先停掉它再装。）', mbError, MB_OK);
      Result := False; Exit;
    end;
    if PortInUse(HttpPort) then
    begin
      MsgBox('Web 端口 ' + IntToStr(HttpPort) + ' 已经被占用，请换一个。', mbError, MB_OK);
      Result := False; Exit;
    end;
    SelectedRtspPort := RtspPort;
    SelectedHttpPort := HttpPort;
  end
  else if CurPageID = CredPage.ID then
  begin
    SelectedUser := Trim(CredPage.Values[0]);
    SelectedPass := CredPage.Values[1];

    { 下面两个是**确认**，不是校验 —— 值在 InitializeWizard 里已经取过一遍。
       静默安装下直接跳过：存下来就是这一条，问出去就是挂住。 }
    if WizardSilent() then Exit;

    if SelectedUser = '' then
    begin
      MsgBox('用户名不能为空。', mbError, MB_OK);
      Result := False; Exit;
    end;
    if SelectedPass = '' then
      if MsgBox('口令留空**不是**关闭鉴权，而是把口令设成空字符串 ——' + #13#10 +
                '任何知道用户名的人都能连上。确定要留空吗？',
                mbConfirmation, MB_YESNO) = IDNO then
      begin
        Result := False; Exit;
      end;
    if SelectedPass = 'admin' then
      if MsgBox('口令还是出厂默认值 admin/admin。' + #13#10 + #13#10 +
                '这台设备会暴露在局域网上，建议现在就改掉。' + #13#10 +
                '确定要用默认口令继续吗？',
                mbConfirmation, MB_YESNO) = IDNO then
      begin
        Result := False; Exit;
      end;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  { 配置写在**装完**这一步，而不是 [Files] 里带一个模板文件 ——
     模板没法把用户在向导里选的端口填进去。 }
  if CurStep = ssPostInstall then
    WriteConfig;
end;

{ 卸载时问一句要不要连配置一起删掉。选"否"就把 config.json 先挪出来 ——
   换版本重装时不用再填一遍端口和凭据。 }
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Cfg, Keep: String;
begin
  if CurUninstallStep = usUninstall then
  begin
    Cfg := ExpandConstant('{app}\config.json');
    if FileExists(Cfg) then
    begin
      { 静默卸载**不问**，直接按"一起删掉"走。理由和上面那些 MsgBox 一样：
         问出去就是挂住，而一个挂住的卸载比丢一份配置难查得多。
         无人值守的场合本来也没有"保留配置"这个概念的落点。 }
      if UninstallSilent() then Exit;

      if MsgBox('要连配置文件一起删掉吗？' + #13#10 + #13#10 +
                '配置里存着端口、监听地址和 ONVIF 用户名口令。' + #13#10 +
                '选"否"会保留它（放在 ' + ExpandConstant('{userappdata}\Desk2ONVIF') + '），' + #13#10 +
                '下次重装不用再填一遍。', mbConfirmation, MB_YESNO) = IDNO then
      begin
        Keep := ExpandConstant('{userappdata}\Desk2ONVIF');
        if not DirExists(Keep) then CreateDir(Keep);
        CopyFile(Cfg, Keep + '\config.json', False);
      end;
    end;
  end;
end;
