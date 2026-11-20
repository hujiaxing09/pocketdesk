; PocketDesk 安装包（Inno Setup 7）。用 installer\build.ps1 构建，它会传入版本号。
; 安装包只负责复制文件；系统服务、防火墙、权限都由 pocketdesk.exe --install / --uninstall 处理。

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{6840F413-4C24-4492-B032-CE80AABA9103}
AppName=PocketDesk
AppVersion={#AppVersion}
AppVerName=PocketDesk {#AppVersion}
AppPublisher=PocketDesk
AppComments=用手机远程控制局域网里的 Windows 电脑
DefaultDirName={autopf}\PocketDesk
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
DefaultGroupName=PocketDesk
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 合成触摸（CreateSyntheticPointerDevice）需要 Windows 10 1809
MinVersion=10.0.17763
OutputDir=..\dist
OutputBaseFilename=PocketDesk-{#AppVersion}-setup
SetupIconFile=..\res\pocketdesk.ico
UninstallDisplayIcon={app}\pocketdesk.exe
UninstallDisplayName=PocketDesk
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; 升级时结束正在运行的 PocketDesk（便携方式启动的那份）
CloseApplications=force
RestartApplications=no

[Languages]
Name: "chs"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[InstallDelete]
; 网页文件整个换掉，避免旧版本留下的文件
Type: filesandordirs; Name: "{app}\web"

[Files]
Source: "..\build\RelWithDebInfo\pocketdesk.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\web\*"; DestDir: "{app}\web"; Flags: ignoreversion recursesubdirs
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
; 从开始菜单打开：确保服务在运行，然后打开扫码配对页
Name: "{group}\PocketDesk"; Filename: "{app}\pocketdesk.exe"; Comment: "打开配对页，用手机扫码连接"
Name: "{group}\卸载 PocketDesk"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\pocketdesk.exe"; Parameters: "--install"; StatusMsg: "正在注册系统服务、设置防火墙…"; Flags: runhidden waituntilterminated
Filename: "{app}\pocketdesk.exe"; Description: "打开配对页，用手机扫码连接"; Flags: postinstall nowait skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{app}\pocketdesk.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveService"

[UninstallDelete]
Type: dirifempty; Name: "{app}"

[Code]
// 还有没有 pocketdesk.exe 在运行（tasklist 的结果交给 find，找到时退出码是 0）
function PocketDeskRunning(): Boolean;
var
  Code: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'),
    '/C tasklist /NH /FI "IMAGENAME eq pocketdesk.exe" | find /I "pocketdesk.exe" >nul',
    '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;

// 升级前先停掉旧版本的服务（服务会让程序退出），否则文件被占用。
// net stop 在服务报告“已停止”时就返回，服务进程本身还要过一会儿才真正退出，所以再等它们都退出（最多 10 秒）。
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code, I: Integer;
begin
  Exec(ExpandConstant('{sys}\net.exe'), 'stop PocketDesk', '', SW_HIDE, ewWaitUntilTerminated, Code);
  for I := 1 to 50 do
  begin
    if not PocketDeskRunning() then
      break;
    Sleep(200);
  end;
  Result := '';
end;

// 卸载最后问一句要不要删除授权记录、证书和日志（静默卸载时保留）
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep <> usPostUninstall then
    exit;
  DataDir := ExpandConstant('{app}\data');
  if not DirExists(DataDir) then
    exit;
  if UninstallSilent then
    exit;
  if MsgBox('是否同时删除这台电脑上 PocketDesk 的授权记录、证书和日志？' + #13#10#13#10 +
            '是：全部删除，不留痕迹。以后重新安装，手机要重新信任并授权这台电脑。' + #13#10 +
            '否：保留在 ' + DataDir + '，重新安装到同一位置后手机不用重新授权。',
            mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
  begin
    DelTree(DataDir, True, True, True);
    RemoveDir(ExpandConstant('{app}'));
  end;
end;
