# 掌控 · PocketDesk（下一代）

在局域网内，用手机浏览器查看并触控操作 Windows 电脑。不用装 App。

这是 [hujiaxing09/pocketdesk](https://github.com/hujiaxing09/pocketdesk) 的重写版。旧版基于 Sunshine + MoonlightWeb，能用但太重。新版的目标是：每台电脑一个小程序，手机直接连接，不需要网关和电脑之间的 PIN 配对。旧版在新版达到同等可用之前继续保留。

> 当前状态：规划阶段，还没有代码。

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
  → Media Foundation 硬件 H.264 编码（NVENC / QuickSync / AMF，低延迟：无 B 帧、按需关键帧）
  → HTTPS 上的 WebSocket
手机浏览器
  → WebCodecs VideoDecoder 硬件解码（iOS 16.4+，必须 HTTPS）
  → canvas 显示
  触控手势 → WebSocket JSON → 电脑上用 SendInput 注入鼠标、滚轮、键盘（中文用 KEYEVENTF_UNICODE）
```

Sunshine 是开源的，它的截屏和低延迟编码参数可以参考。

## 第一次开发会话要先决定的事

1. **开发语言**：C++（MSVC，直接调用 D3D11 和 Media Foundation），还是 C#（.NET 8 + Vortice.Windows）。旧版用系统自带的 C# 5 编译器，写 COM/D3D11 代码会很吃力。
2. **HTTPS 证书**：WebCodecs 只能在 HTTPS 页面里使用。可选：每台电脑自签证书（每台电脑每部手机都要确认一次）、一台入口电脑统一提供页面、或给手机安装自建的根证书。
3. **多台电脑的手机授权**：每台电脑各自扫码，还是授权一次后在多台电脑间共享。
4. **许可证**。

## 分阶段计划

每个阶段都有验收标准。上一阶段由用户用 iPhone 在真实电脑上验收通过后，再进入下一阶段。

| 阶段 | 内容 | 验收标准 |
| --- | --- | --- |
| 0 打通画面 | 截屏 → 硬件 H.264 → WebSocket → iPhone WebCodecs 显示，先不做输入 | 在 .53 上用 iPhone 看到桌面，测出延迟、帧率、CPU/GPU 占用 |
| 1 单机可用 | 触控输入（复用旧版手势层）、中文输入、光标、断线重连、高 DPI 和多显示器坐标、码率自适应、扫码授权、托盘程序、日志 | 在一台电脑上能完整替代旧版 |
| 2 多台电脑 | 局域网发现、主机切换、授权方案 | .53 和 .233 都能从同一个页面操作 |
| 3 安装包 | 安装、卸载（可删除记录）、防火墙规则、开机启动 | 安装和卸载都在干净的电脑上验证 |
| 4 可选 | 锁屏和 UAC 弹窗下也能操作（系统服务 + 会话内进程） | 按需决定 |

## 已知的坑

- 程序以普通权限运行时，Windows 不允许它操作以管理员身份运行的窗口（例如任务管理器）。
- 高 DPI 和多显示器下坐标换算容易点偏。
- iPhone Safari 切到后台会断开 WebSocket，必须自动重连。
- 不同显卡的硬件编码器行为不同（支持的档次、码率控制、关键帧请求），需要在 NVIDIA、Intel、AMD 上各测一次。
- WebCodecs 需要正确的 H.264 配置（Annex B 和 avcC 格式、SPS/PPS 随关键帧下发）。

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
