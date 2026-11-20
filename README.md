# 掌控 · PocketDesk（下一代）

在局域网内，用手机浏览器查看并触控操作 Windows 电脑。不用装 App。

这是 [hujiaxing09/pocketdesk](https://github.com/hujiaxing09/pocketdesk) 的重写版。旧版基于 Sunshine + MoonlightWeb，能用但太重。新版的目标是：每台电脑一个小程序，手机直接连接，不需要网关和电脑之间的 PIN 配对。旧版在新版达到同等可用之前继续保留。

> 当前状态：阶段 0（打通画面）已于 2026-11-20 用 iPhone 验收通过，下一步是阶段 1。

## 需求

- 手机（主要是 iPhone Safari）和被控电脑在同一个局域网。
- 看到电脑桌面，用触控完成点击、双击、拖动、右键、滚轮，能打字（包括中文）。
- 可以控制多台电脑，在页面上切换。
- 画面要接近 Sunshine 的流畅度，所以**从一开始就用硬件 H.264**，不用截屏加 JPEG 的方案。
- 安装包可以安装和卸载，卸载时可以删除使用记录，可以查看日志。

## 为什么不用 Sunshine + MoonlightWeb

Sunshine 是游戏串流服务器，MoonlightWeb 在电脑上跑一个 Moonlight 客户端再转成 WebRTC。对这个需求来说，代价包括：网关和每台主机之间的 PIN 配对、手机的二次授权、用字符串匹配给上游 JS 打补丁、约 90 MB 的安装包，以及入口电脑成为单点。旧仓库里大量代码都在处理配对和证书问题。

## 技术方案

```
每台被控电脑：PocketDesk 程序
  DXGI Desktop Duplication 截屏（画面留在显卡里）
  → FFmpeg 调硬件 H.264 编码（NVENC / QuickSync / AMF，低延迟：无 B 帧、按需关键帧；做法照 Sunshine）
  → WebRTC 视频轨（libdatachannel：RTP 打包，响应手机的重传请求 NACK 和关键帧请求 PLI）
手机浏览器（Safari，页面必须是 HTTPS）
  → RTCPeerConnection 接收，<video> 硬件解码显示
  触控手势 → WebRTC DataChannel JSON → 电脑上用 SendInput 注入鼠标、滚轮、键盘（中文用 KEYEVENTF_UNICODE）
  信令：页面通过 HTTPS 请求和电脑交换 SDP
```

原则：尽量向 Sunshine 靠拢。Sunshine 是 GPL-3.0，本项目也是，截屏、编码参数、输入注入的代码可以直接搬。注意 Sunshine 本身不用 WebRTC（它用 Moonlight 私有协议：RTSP 建会话，UDP 上的 RTP 加 FEC 传视频，ENet 传控制和输入）；浏览器不能直接收发 UDP，WebRTC（也是 UDP 上的 RTP）是浏览器里离它最近的做法。

## 已决定的事（第一次开发会话，2026-11-20）

1. **开发语言：C++（MSVC）**。直接调用 D3D11 和 DXGI，与 Sunshine 一致，程序体积小。代价是 HTTPS、WebRTC、证书生成、托盘这些要靠引入库或者自己写。
2. **HTTPS 证书：每台电脑各自生成根证书**。根证书带名称约束，只能签局域网 IP 和 `.local` 名称；电脑用它签自己的服务器证书，IP 变了就自动重签。手机给每台电脑装一次描述文件，再到"证书信任设置"里打开。各台电脑不共用根证书，因为共用就要在电脑之间传私钥，等于又加回了配对。iOS 对服务器证书的要求：有效期不超过 825 天、必须有 SAN、EKU 包含 serverAuth。
3. **手机授权：每台电脑各自扫码**。每台电脑是独立的源，浏览器存的令牌不能跨电脑读取。授权一次多台共享需要页面同源（入口电脑）或者电脑之间配对，两者都不要。同一个二维码同时完成安装证书和授权。
4. **许可证：GPL-3.0**。和旧仓库一致，可以直接使用 Sunshine（GPL-3.0）的代码。
5. **HTTPS 服务：cpp-httplib + OpenSSL**。视频和输入都走 WebRTC，HTTPS 只需提供页面和交换 SDP，单头文件的 cpp-httplib 就够用，不需要 WebSocket，也不用编译 Boost。OpenSSL 同时负责生成带名称约束的根证书和服务器证书。没有选 Windows 自带的 http.sys + Schannel，因为它绑定证书和 URL 需要管理员权限。
6. **构建：CMake + vcpkg 清单模式**。依赖写在 `vcpkg.json` 里，triplet 用 `x64-windows-static`（静态链接，安装后不依赖 VC++ 运行库）。用 VS 2026 自带的 CMake 和 vcpkg 即可，不需要另外安装。
7. **视频传输：WebRTC（服务端用 libdatachannel）**。视频走 WebRTC 视频轨，触控和键盘走同一连接里的 DataChannel。理由：
   - 走 UDP，丢一个包只影响那一帧，不会像 TCP 那样让后面的帧都等重传；自带重传请求（NACK）、关键帧请求（PLI）和拥塞反馈。Moonlight 也是走 UDP。
   - 旧版 MoonlightWeb 到手机这一段就是 WebRTC，已经在这台 iPhone 上证明流畅度可以接受。
   - 解码交给 Safari 自己的 WebRTC 管线，不依赖 WebCodecs。
   - 代价：连接建立（ICE/DTLS/SRTP）复杂、难调试；播放缓冲由 Safari 决定，延迟没法自己压；Safari 上同样要 HTTPS（非安全页面不做 ICE），证书方案不变。
   - 比较过的另外两种：WebSocket + WebCodecs（最简单、延迟全由自己控制，但 Wi-Fi 丢包重传时会整体卡顿）；WebTransport（iOS 26.4 才支持，C++ 服务端没有成熟的库）。
   - 阶段 0 要测端到端延迟。如果 Safari 的播放缓冲让延迟太高，停下来商量；备选是用 DataChannel 传 H.264、页面里用 WebCodecs 自己解码。
8. **硬件编码：FFmpeg（与 Sunshine 一致）**。用 FFmpeg 的 h264_nvenc / h264_qsv / h264_amf，截屏得到的 D3D11 纹理直接交给编码器，各家显卡的低延迟参数照搬 Sunshine。没有选 Media Foundation，因为各家显卡的 MF 编码器可调的低延迟选项少且不一致，Sunshine 的代码和参数也用不上。FFmpeg 只编译 avcodec 和这三个硬件编码器，不带 GPL 和 nonfree 组件。

## 分阶段计划

每个阶段都有验收标准。上一阶段由用户用 iPhone 在真实电脑上验收通过后，再进入下一阶段。

| 阶段 | 内容 | 验收标准 |
| --- | --- | --- |
| 0 打通画面 | 截屏 → 硬件 H.264 → WebRTC → iPhone Safari 显示，先不做输入 | 在 .53 上用 iPhone 看到桌面，测出延迟、帧率、CPU/GPU 占用 |
| 1 单机可用 | 触控输入（复用旧版手势层）、中文输入、光标、断线重连、高 DPI 和多显示器坐标、码率自适应、扫码授权、托盘程序、日志 | 在一台电脑上能完整替代旧版 |
| 2 多台电脑 | 局域网发现、主机切换、授权方案 | .53 和 .233 都能从同一个页面操作 |
| 3 安装包 | 安装、卸载（可删除记录）、防火墙规则、开机启动 | 安装和卸载都在干净的电脑上验证 |
| 4 可选 | 锁屏和 UAC 弹窗下也能操作（系统服务 + 会话内进程） | 按需决定 |

## 阶段 0 结果（2026-11-20）

测试环境：被控电脑是 RTX 3050 笔记本，主屏 2560×1440@165Hz，NVENC 编码；手机是 iPhone，iOS 18.7 Safari，同一个 Wi-Fi。测试时主屏上放一个持续变化的动画窗口。

| 项目 | 结果 |
| --- | --- |
| 画面 | 2560×1440，H.264 Constrained Baseline，CBR 12 Mbps |
| 手机解码帧率 | 59–60 fps，丢帧 0 |
| 网络 | 往返 5–7 ms，丢包 0，重传请求 0，关键帧请求 0 |
| 手机缓冲 | 约 4.3 ms（Safari 接受了 playout-delay 扩展，基本不缓冲） |
| 手机解码 | 约 5.4 ms |
| 延迟估算 | 约 25 ms = 电脑（等帧 + 转换 + 编码）约 12 + 网络 3 + 缓冲 4 + 解码 5，不含手机屏幕显示 |
| 电脑编码 | NVENC 约 4.5 ms/帧 |
| 电脑占用 | CPU 1.5%（16 线程总量，约合单核 24%），GPU 视频编码引擎 21%，3D 引擎 6% |
| 断线重连 | 电脑端程序重启后，手机约 1 秒内自动重连 |

说明：画面静止时没有新帧，帧率会降到个位数，Safari 会把这算作“卡顿”（freezeCount），不是故障。延迟是各段相加的估算，没有做拍屏的端到端实测。

## 已知的坑

- 程序以普通权限运行时，Windows 不允许它操作以管理员身份运行的窗口（例如任务管理器）。
- 高 DPI 和多显示器下坐标换算容易点偏。
- iPhone Safari 切到后台会断开连接，必须自动重连（WebRTC 要重新协商）。
- 不同显卡的硬件编码器行为不同（支持的档次、码率控制、关键帧请求），需要在 NVIDIA、Intel、AMD 上各测一次。
- Safari 的 WebRTC 对 H.264 有要求：档次要在 SDP 里协商一致（profile-level-id）、packetization-mode=1、SPS/PPS 随每个关键帧下发。
- NVENC 头文件（ffnvcodec）的版本决定最低 NVIDIA 驱动版本。vcpkg 默认的 13.1 要求 610 以上的驱动，在 API 13.0 的驱动上直接打不开编码器，所以 `vcpkg.json` 里固定为 11.1.5.3（驱动 471.41 以上）。
- NVIDIA 上 FFmpeg 建 NV12 纹理数组并带 RENDER_TARGET 会报 `E_INVALIDARG`，编码纹理池改为每帧单独一张纹理。

## 构建和运行

需要 Visual Studio 2026（含 C++ 桌面开发），它自带 CMake 和 vcpkg。在 PowerShell 里：

```powershell
$env:VCPKG_ROOT = "E:\Visual Studio8\Community\VCcpkg"   # 按实际安装位置
cmake --preset default
cmake --build --preset release
.uild\RelWithDebInfo\pocketdesk.exe
```

第一次配置会用 vcpkg 编译依赖，FFmpeg 要 20 分钟左右。程序启动后会打印两类地址：

- `http://<电脑IP>:47080/`：首次设置页，手机在这里下载并信任这台电脑的根证书（每部手机每台电脑一次）。
- `https://<电脑IP>:47443/`：画面。点屏幕显示或隐藏统计（帧率、码率、延迟估算、丢包）。

端口：TCP 47443（HTTPS）、47080（HTTP），UDP 47500–47599（WebRTC）。第一次运行时 Windows 防火墙会弹窗，需要允许“专用网络”。证书存在 `%LOCALAPPDATA%\PocketDesk\certs`。

常用参数：`--list`（列出显示器）、`--output N`、`--encoder h264_nvenc|h264_amf|h264_qsv`、`--fps 60`、`--bitrate 12000`（kbps）、`--web DIR`（直接用源码里的 web 目录，改网页不用重新编译）。

本机调试：`http://127.0.0.1:47080/app/` 只允许本机访问，浏览器把 127.0.0.1 当作安全页面，不装证书也能测整条链路。

## 从旧仓库复用

旧仓库提交 `3594378` 中，这些文件不依赖 MoonlightWeb，可以直接拿来：

- `web/gestures.js`：触控手势（点击、双击、拖动、长按右键、双指滚动和平移、三指键盘）
- `web/keyboard.js`：手机键盘输入
- `test/gestures.test.js`：手势测试
- `web/icon.ico`、`web/icon-256.png`：图标

## 工作方式

- 每阶段先确认要做什么、怎么验收，做完交给用户测。
- 遇到技术限制或方向问题，停下来说明情况并商量，不要自己绕路解决。
- 不提前堆功能。
