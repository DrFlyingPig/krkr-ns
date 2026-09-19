<div align="center">

# KRKR-ns

**吉里吉里（KiriKiri）视觉小说引擎 · Nintendo Switch 移植版**

**`⚠️ 实验性项目，不保证稳定性 ⚠️`**

基于 [krkrsdl2](https://github.com/krkrsdl2/krkrsdl2)（pinned `bf207f2`）· 内嵌 krkrsdl2/krkrz（pinned `b11c43a`）

[![Release](https://img.shields.io/github/v/release/DrFlyingPig/krkr-ns?style=flat-square)](https://github.com/DrFlyingPig/krkr-ns/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-red?style=flat-square)]()

**[⬇️ NRO下载入口](https://github.com/DrFlyingPig/krkr-ns/releases/latest)**

</div>

---

## 📖 说明介绍

KRKR-ns 将 PC 端吉里吉里（KiriKiri / KRKR）引擎完整移植到 Nintendo Switch，可直接运行未加密 `.xp3` 打包的 KRKR / KAG 视觉小说，也支持展开的目录式游戏。

内置游戏库启动器：扫描 `sdmc:/switch/KRKR-ns/Game/`，自动挑选入口 xp3（`启动游戏.xp3` > `运行游戏.xp3` > `data.xp3` …），显示封面并支持自定义名称；列表、翻页、弹窗均可触摸点击，按键与触摸都有按下反馈。游戏内选择「结束游戏」会直接**返回启动器**，可以立刻更换下一款游戏，无需退出程序——模拟器与真机均以同进程整引擎重建获得全新引擎。

## ⬇️ 获取与使用

1. 从 [Releases](https://github.com/DrFlyingPig/krkr-ns/releases/latest) 下载 `krkrsdl2.nro`，放到 SD 卡的 `sdmc:/switch/KRKR-ns/` 目录下。
2. **真机**：进入 HBMenu 加载（相册方式需按住 `R` 进入 title-override）；**模拟器**：直接加载该 nro（Ryujinx 系 fork 均可）。
3. 游戏放到 `sdmc:/switch/KRKR-ns/Game/<游戏目录>/`，启动器会列出其中的 `.xp3`，选择即可开始；同目录的其余 xp3 会自动挂载为资源包。
4. 存档按游戏隔离存放于 `sdmc:/switch/KRKR-ns/saves/<游戏目录>/`。

> ⚠️ **特别说明：Switch 的文件路径不支持中文。**
> 真机上游戏目录名与 xp3 文件名必须使用英文 / 数字等 **ASCII 字符**（如 `sdmc:/switch/KRKR-ns/Game/CafeStella/play.xp3`）。自己找来的游戏资源若带有中文文件名或中文名目录（含补丁、追加包），请**先全部重命名为 ASCII** 再放入，否则真机无法识别。
> 模拟器走 PC 文件系统不受此限制，但建议统一使用 ASCII 命名，避免同一份资源两端行为不一致。

> 诊断日志按次保存在 `sdmc:/switch/KRKR-ns/log/`（每次启动一个文件，自动只保留最新 3 份）；运行时开关（标记文件，置于 `sdmc:/switch/KRKR-ns/` 下）与全部源码级补丁清单见 [PATCHES.md](docs/PATCHES.md)。
> 自行构建：devkitPro 工具链（`NINTENDO_SWITCH=ON`），`build_nro.sh` 一键完成构建、打包与模拟器部署。

## ✅ 已实现功能

> 尚未覆盖的插件族与格式缺口（对照 krkrsdl3 盘点）、移植优先级见 [COMPAT_BACKLOG.md](docs/COMPAT_BACKLOG.md)。

**启动器与多游戏管理**

- 游戏库界面：封面（自定义 / 自动生成）、名称别名（`Names.tjs`）、入口 xp3 自动挑选、启动文件选择（X）、退出确认（A）、重扫（Y）
- 全触摸操作：列表行 / 上下列表 / 翻页 / 弹窗条目 / 按钮均有触摸命中区；按键与触摸的按下反馈一致
- 游戏内「结束游戏」→ 返回内置启动器，连续更换游戏（真机链式重启 / 模拟器同进程重建）
- 内存模式提示与「重试」：非完整内存模式给出 HBMenu 启动指引；启动过程分阶段 `[boot]` 计时
- 存档按游戏目录隔离；compat 补丁目录（SD 卡）可覆盖内置适配层

**引擎兼容与适配**

- KAG2 / KAG3 兼容层（Switch 桩替换桌面版插件脚本，自动维持优先级）
- KAG3 的二次窗口确认框可用：对话框寄宿在主窗口内呈现（居中叠加），输入坐标换算到对话框自身坐标系，并支持模态循环——快速存/读档等确认框不再中断动作
- 菜单 API 与 `menu.dll` 语义对齐：没有该插件时不向脚本暴露原生 `MenuItem` / `Window.menu`（KAGEX 作品保持自己的 TJS 菜单模型，KAG3 需要的脚本侧菜单模型只发给不带兼容层的作品）
- TJS 引擎多线程安全：每线程独立寄存器区栈（加密包的解码引擎在音频线程执行脚本时，不再与主线程互相踩坏寄存器）
- 常用插件内置化：E-mote（emoteplayer / motionplayer）、psbfile、textrender、kagparser、csvparser、layerExBTOA 等；游戏对插件的文件存在性探测对内置插件生效，E-mote 等子系统正常启用
- 内置插件再扩充：`dirlist`（getDirList）、`getabout`、`getsample`（唇同步 / 波形采样）、`savestruct`（文本存档格式）、`varfile`（`var://`）、`win32dialog`、`addfont`、`fftgraph`、`wutcwf`（TCWF 音频解码）
- 文本编码探测：无 BOM 文本按 UTF-8 → Shift-JIS → GBK 依次尝试，中文重编码脚本不再中断启动
- 引擎 API 补齐：`Layer.affinePile`、`Layer.stitchWrappedCopy`、`Font.doUserSelect`；缺失转场自动回落 crossfade
- 加密 xp3 数据包支持：自动加载游戏目录下的 `xp3filter.tjs` 解密过滤器（Kirikiroid2 兼容，独立脚本引擎逐块解密，附原生 XOR 快路径）
- KAGEX 适配：`Window.fullScreen` 控制台语义（避免 Windows 专属全屏流程导致白屏）、方屏扩展画布（exHeight）按顶部可见区等比满屏呈现；经典 4:3 画布（1024×768 等）整幅 letterbox 显示，消息窗口不再被裁掉（两种呈现共用同一判定，纹理上传的行带与之保持一致）
- 缺失资源垫图、`Layer.loadImages` 容错、脚本异常记录并继续（风暴熔断）

**图形与性能**

- E-mote 立绘动画：隔离 GL 后端 + 驱动自检（异常驱动自动回退 CPU）、脏区回读、半分辨率可选
- simde SIMD 混合内核（NEON）、4 核绘制线程池
- GPU 呈现链直通（FBO blit）、XP3 段缓存、增量资源索引、解码图像缓存（上限 96 MiB）
- 帧内归因埋点（`[prof] frame:` / `[prof] blt:`）、合成层脏区上传、上传统一行带与呈现裁剪共用同一判定
- 菜单公共路径优化：字体度量按需缓存、存档缓冲写入、PSB 共享资源、固实 7z 解压块复用
- 启动器重绘优化：文本宽度缓存 + 按样式分组绘制，无输入时不重绘
- 大位图独立内存区复用；修复文字对象销毁影响其他字体、选项文字垂直居中的问题

**视频播放**

- FFmpeg 解码管线（SwitchMovieOverlay）：ASF / MOV/MP4 / MPEG-PS 容器按签名自动识别，支持归档内与目录式视频源；子集构成见 `tools/build_ffmpeg_switch.sh`
- 独立解码线程（4 MiB 栈）+ 双缓冲帧；**layer / overlay / mixer 三种模式均可上屏**（overlay 按脚本设定区域叠画在场景上方）
- 音轨解码：WMA / AAC / MP2 / MP3 等经 swresample 下混为 S16，通过引擎音频设备（FAudio）输出；视频结束事件等待声音播完（逐块排空 PCM 环），音量可调

## 🗂 架构

引擎分四层：内嵌上游引擎核心 → Switch 平台层（本项目主要改动区）→ 内置插件 → 脚本兼容层。仓库布局：

```
KRKR-ns/                            # 仓库根
├── krkrsdl2/                       # 引擎源码（基于 krkrsdl2，pinned bf207f2）
│   ├── src/                        # Switch 平台层 + 内置插件（本项目主要改动区）
│   │   ├── core/sdl2/              # SDLApplication、GL 合成、KrkrNS 日志/路径/剖析
│   │   ├── core/base/sdl2/         # 存储、脚本管理、系统、插件装载（PluginImpl）、7z
│   │   ├── core/visual/sdl2/       # 绘制设备、Layer、视频 overlay（SwitchMovieOverlay）
│   │   ├── core/sound/sdl2/        # 音频设备（FAudio）与波形解码（Vorbis / Opus）
│   │   ├── core/environ/sdl2/      # 事件循环、窗口、线程、CPU 探测
│   │   ├── core/msg/sdl2/          # 消息对话框
│   │   ├── core/utils/sdl2/        # 剪贴板等
│   │   ├── plugins/                # 内置插件：emoteplayer、psbfile、kagparser、textrender…
│   │   ├── resources/nswitch/      # 平台资源（图标）
│   │   └── config/                 # 源文件清单（构建系统使用）
│   ├── external/                   # 内嵌上游与第三方（krkrz 引擎核心 pinned b11c43a、SDL2、FAudio、simde、zlib）
│   ├── data/                       # 启动器 startup.tjs 与内置字体
│   ├── CMakeLists.txt              # 构建入口（NINTENDO_SWITCH=ON）
│   └── meson.build
├── docs/                           # 文档（开发记录）
├── compat-patches/                 # TJS 兼容垫片（部署进 romfs）
├── tools/                          # 构建 / 调试脚本（build_ffmpeg_switch.sh、upstream_delta.sh 等）
├── tests/                          # 单元测试（位图桥、E-mote GL 等）
└── build_nro.sh                    # 一键构建 / 打包 / 模拟器部署
```

> 文档索引：[docs/README.md](docs/README.md)——兼容性缺口 [COMPAT_BACKLOG.md](docs/COMPAT_BACKLOG.md)、源码级补丁 [PATCHES.md](docs/PATCHES.md)、上游差异 [UPSTREAM_DELTA.md](docs/UPSTREAM_DELTA.md)（`tools/upstream_delta.sh` 自动生成）。

## 📄 许可

本仓库以 [krkrsdl2 的 MIT 许可](LICENSE) 发布；内嵌/引用的上游组件（krkrz 引擎、FAudio、SDL2 及其 Switch 端口、simde、zlib、FreeType、libjpeg-turbo、libpng、libogg/libvorbis、libopus 等）各自保留其原始许可证与归属，见各组件目录内的 LICENSE/COPYING 文件。

**本项目不包含、也不分发任何商业游戏资源。**
