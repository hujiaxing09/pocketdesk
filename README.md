# 掌控 · PocketDesk

用手机浏览器查看并操作局域网里的 Windows 电脑。手机不用装 App，电脑装一个小程序就行。

- 画面用显卡硬件编码、WebRTC 传输，60 帧，延迟约 25 毫秒（局域网、iPhone 实测）
- 支持 iPhone（Safari）和安卓（Chrome、Edge），可以添加到主屏幕全屏使用
- 多台电脑组成一个组：手机授权一次，组里每台电脑都能用；锁屏界面、UAC 弹窗也能操作

## 功能

- **两种操作方式**：触控模式和 Windows 平板一样（点按、长按右键、拖动、双指缩放和滚动）；触控板模式像笔记本触控板一样移动光标，下方有左键、右键按钮，默认不在手机上显示画面（看电脑自己的屏幕，电脑不传画面更省电）。
- **键盘**：支持中文输入法，另有 Esc、Tab、方向键、Del 和 Ctrl / Alt / Shift / ⊞ 组合键。
- **电脑组**：多台电脑可以组成一个组，共用证书和手机授权。手机只需信任一次证书、授权一次，就能在菜单里切换组里的每台电脑；新电脑加入组后，组里的手机马上就能用它。可以给每台电脑起个只在手机上显示的名字。
- **扫码授权**：手机扫一下电脑上的二维码，或者输入 6 位数字。授权一直有效，直到在任意一台组里的电脑上移除。
- **锁屏也能用**：作为系统服务运行，开机还没登录、锁屏、UAC 弹窗、以管理员身份运行的窗口都能看到和操作。
- **画面设置**：多显示器切换，画质（自动码率 / 省流 / 清晰 / 高清），分辨率和帧率。
- **不用时不占资源**：手机锁屏、切到别的 App 或一段时间不操作就自动断开，电脑停止编码；回来时自动重连。
- **干净卸载**：证书、授权记录和日志都在安装目录的 `data` 文件夹里，卸载时可以一起删掉。

## 系统要求

| | 要求 |
| --- | --- |
| 电脑 | Windows 10（1809 及以上）或 Windows 11，64 位 |
| 显卡 | 能硬件编码 H.264 的 NVIDIA 或 AMD 显卡，或者 Intel 核显（Quick Sync）；不需要独立显卡 |
| 显示器 | 需要有显示器（或 HDMI 显卡欺骗器），见[常见问题](#常见问题) |
| 手机 | iPhone（Safari），或安卓手机（Chrome、Edge） |
| 网络 | 手机和电脑在同一个局域网 |

## 安装

1. 从 [Releases](https://github.com/hujiaxing09/pocketdesk/releases) 下载最新的安装包 `PocketDesk-<版本>-setup.exe`（也可以按下面的[从源码构建](#从源码构建)自己打包）。
2. 在电脑上运行安装包，可以选择安装位置。安装后 PocketDesk 作为系统服务运行，开机自动启动，防火墙自动放行。
3. 安装完成时会打开**配对页**，上面有二维码和 6 位数字。以后可以点右下角托盘里的 PocketDesk 图标，或者从开始菜单打开 PocketDesk 再次打开它。

## 连接手机

1. 用手机相机扫描配对页上的二维码，按提示在浏览器里打开。
2. 第一次连接时，手机要信任电脑的证书（同一个组的电脑共用证书，每部手机只做一次），页面会给出两种方式：
   - **直接打开**：浏览器提示“连接不是私密连接”时，iPhone 点“显示详细信息 → 访问此网站”，安卓 Chrome 点“高级 → 继续前往”。
   - **安装证书**（更稳定）：按页面上的步骤下载并安装这台电脑的证书，以后不会再提示。
3. 授权完成后就能看到电脑桌面。
4. 建议添加到主屏幕：iPhone 在 Safari 里点“共享 → 添加到主屏幕”，安卓在 Chrome 菜单里点“添加到主屏幕”或“安装应用”。从主屏幕图标打开是全屏的。

**添加更多电脑**：在新电脑上装好 PocketDesk，打开它的配对页，在“电脑组”一栏里找到局域网里已有的电脑，点“加入”，输入那台电脑配对页上的 6 位数字。加入后新电脑会自动重启几秒，然后就出现在手机的电脑列表里，不用再授权、也不用再信任证书。

## 使用

画面旁边有两个按钮：**键盘**和**菜单**。没有黑边时它们浮在屏幕边上，可以拖动。

菜单里可以切换电脑、选择操作方式、切换显示器、调整画质、设置多久不操作自动断开，或者手动断开连接。

| 操作 | 触控模式 | 触控板模式 |
| --- | --- | --- |
| 单击 | 点一下 | 轻点 |
| 右键 | 长按 | 双指轻点，或点右键按钮 |
| 拖动 | 按住拖 | 长按后拖，点一下再按住拖，或按住左键按钮、另一根手指拖 |
| 右键拖动 | 双指拖 | 按住右键按钮，另一根手指拖（和笔记本一样） |
| 滚动 | 双指滑动 | 双指滑动 |
| 缩放 | 双指捏合 | — |
| 移动光标 | — | 单指滑动 |
| 打开键盘 | 键盘按钮 | 键盘按钮，或三指轻点 |

触控板模式下屏幕底部是左键、右键两个按钮，按下时变色并轻微振动（安卓，以及 iOS 18 起的 iPhone）。手机上默认不显示电脑画面，要显示可以在菜单里打开“显示电脑画面”。

键盘上方有一排特殊键。Ctrl、Alt、Shift、⊞ 点一下会锁定，和下一个按键一起按下（例如 Ctrl 然后 c 就是复制）；锁定后再点一次，就单独按一下这个键（例如点两下 ⊞ 打开开始菜单）。

## 管理和卸载

- **托盘图标**：手机扫码连接（打开配对页）、打开日志文件夹、退出 PocketDesk。退出后手机就连不上这台电脑了，直到下次开机，或者从开始菜单再次打开 PocketDesk。
- **移除手机**：配对页下方列出已授权的手机，点“移除”后那部手机会立即断开，组里所有电脑都不再接受它，需要重新授权才能再连。
- **管理电脑组**：配对页的“电脑组”一栏显示组里的电脑和是否在线，可以把某台电脑移出组，或者让这台电脑退出组。离开组的电脑变回单独的一台，手机要重新授权、重新信任证书才能连它。组里的记录在电脑之间自动同步，关着机的电脑下次开机时会补上。
- **卸载**：在“设置 → 应用”里卸载 PocketDesk。系统服务和防火墙规则会一起删除，最后会问是否删除授权记录、证书和日志。

## 常见问题

**画面是黑的**

手机上会显示原因，常见的有：

- 电脑没有接显示器：没有显示器就没有画面可截。只接一根 HDMI 线不够，可以插一个 HDMI 显卡欺骗器（假显示器）。
- 正在被远程桌面使用，并且远程桌面窗口被最小化了：Windows 会暂停这个画面。把远程桌面窗口还原或者断开就好。想最小化也不黑屏，可以在发起远程桌面的那台电脑上运行：

  ```powershell
  New-ItemProperty -Path "HKCU:\Software\Microsoft\Terminal Server Client" -Name RemoteDesktop_SuppressWhenMinimized -PropertyType DWord -Value 2 -Force
  ```

- 硬件编码启动失败：先更新显卡驱动；具体原因在电脑上 PocketDesk 的日志里（安装目录下的 `data\logs`）。

**锁屏时选了扩展屏是黑的**：Windows 只在主显示器上显示锁屏，切到主显示器即可。

**手机连不上**：确认手机和电脑在同一个局域网；如果没用安装包而是直接运行的 `pocketdesk.exe`，第一次运行时防火墙弹窗要勾选电脑当前的网络类型（包括“公用网络”）。

**用到的端口**：TCP 47443（HTTPS，也用于组内同步）、47080（HTTP 设置页），UDP 47500–47599（WebRTC）、47100（局域网发现）。

## 技术栈

```
电脑（Windows 服务 + 会话内程序，C++）            手机浏览器
  DXGI 截屏 → D3D11 转 NV12 → FFmpeg 硬件编码  ──WebRTC 视频──▶  <video> 硬件解码
  合成触摸 / SendInput 注入输入  ◀──WebRTC DataChannel──  触控、触控板、键盘
  HTTPS 页面和信令、局域网发现  ◀──────HTTPS──────▶  页面、授权、切换电脑
```

| 部分 | 使用的技术 |
| --- | --- |
| 截屏 | DXGI Desktop Duplication，D3D11 视频处理器转 NV12，GPU 合成鼠标指针 |
| 编码 | FFmpeg：NVIDIA NVENC、AMD AMF、Intel Quick Sync，低延迟 H.264 |
| 传输 | WebRTC（[libdatachannel](https://github.com/paullouisageneau/libdatachannel)）：视频轨 + DataChannel |
| 网页服务 | [cpp-httplib](https://github.com/yhirose/cpp-httplib) + OpenSSL，电脑组共用的根证书 |
| 输入 | Windows 合成触摸（Synthetic Pointer）和 SendInput |
| 系统服务 | Windows 服务，在当前会话里以 SYSTEM 启动程序，可以操作锁屏和 UAC |
| 手机页面 | 原生 JavaScript（ES 模块），Service Worker 离线缓存，[jsQR](https://github.com/cozmo/jsQR) 扫码 |
| 构建 | C++20（MSVC）、CMake、vcpkg，安装包用 [Inno Setup](https://jrsoftware.org/isinfo.php) |

截屏、编码参数、触摸注入和系统服务的做法参考了 [Sunshine](https://github.com/LizardByte/Sunshine)。

## 从源码构建

需要 Visual Studio 2026（含“使用 C++ 的桌面开发”，自带 CMake 和 vcpkg）。在 PowerShell 里：

```powershell
$env:VCPKG_ROOT = "<Visual Studio 安装目录>\VC\vcpkg"
cmake --preset default
cmake --build --preset release
```

第一次构建会用 vcpkg 编译依赖，FFmpeg 大约要 20 分钟。生成的程序在 `build\RelWithDebInfo\pocketdesk.exe`，可以直接运行（不安装时锁屏下不能用）。

打安装包需要 [Inno Setup 7](https://jrsoftware.org/isdl.php)：

```powershell
powershell -File installer\build.ps1
```

安装包输出到 `dist\PocketDesk-<版本>-setup.exe`。

设计决定、实现细节和踩过的坑见 [开发说明](docs/development.md)，电脑组的设计见 [电脑组设计](docs/pc-group.md)。

## 致谢

- [Sunshine](https://github.com/LizardByte/Sunshine)：截屏、编码、输入注入和系统服务的参考实现
- [FFmpeg](https://ffmpeg.org/)、[libdatachannel](https://github.com/paullouisageneau/libdatachannel)、[cpp-httplib](https://github.com/yhirose/cpp-httplib)、[OpenSSL](https://www.openssl.org/)、[nlohmann/json](https://github.com/nlohmann/json)、[QR Code generator](https://github.com/nayuki/QR-Code-generator)、[jsQR](https://github.com/cozmo/jsQR)、[Inno Setup](https://jrsoftware.org/isinfo.php)

## 许可证

[GPL-3.0](LICENSE)
