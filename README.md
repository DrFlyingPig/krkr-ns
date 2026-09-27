<div align="center">

# KRKR-ns

**吉里吉里（KiriKiri）视觉小说引擎 · Nintendo Switch 移植版**

**`⚠️ 属实验性项目，难以保证稳定性 ⚠️`**

移植基线：[krkrsdl2](https://github.com/krkrsdl2/krkrsdl2) `bf207f2` · [krkrz](https://github.com/krkrsdl2/krkrz) `b11c43a`；兼容实现参考 Kirikiroid2。

[![Release](https://img.shields.io/github/v/release/DrFlyingPig/krkr-ns?style=flat-square)](https://github.com/DrFlyingPig/krkr-ns/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-red?style=flat-square)

**[⬇️ NRO下载入口](https://github.com/DrFlyingPig/krkr-ns/releases/latest)**

</div>

---

## 📖 说明介绍

KRKR-ns 是面向 Nintendo Switch 的吉里吉里（KiriKiri / KRKR）视觉小说引擎移植项目，基于 krkrsdl2，并参考 Kirikiroid2 改善兼容性。当前启动器以 `.xp3` 为游戏入口，可读取资源包及游戏目录中的文件；部分插件、格式和功能仍有兼容性限制。

内置浅色游戏库界面，支持手柄和触摸操作，可为每个游戏设置预览图、头像和显示名称。切换游戏时只更新界面，点击「开始游戏」后再加载游戏资源；游戏内选择「结束游戏」会返回启动器，方便继续更换游戏。

## ⬇️ 获取与使用

> ⚠️ **重要：Switch 真机的游戏路径不能包含中文。**
>
> 游戏目录、入口 `.xp3` 文件名，以及实际位于 SD 卡上的游戏资源路径都必须遵守此限制。请使用英文、数字、下划线等 ASCII 名称，例如 `sdmc:/switch/KRKR-ns/Game/Game01/data.xp3`。
>
> 更改游戏目录名会影响存档和图片设置的匹配，改名前请先备份存档。不要随意重命名 XP3 包内部资源；需要改名的散装资源，也应同步检查脚本中的文件引用。

1. 从 [Releases](https://github.com/DrFlyingPig/krkr-ns/releases/latest) 下载 `krkrsdl2.nro`，放到 SD 卡的 `sdmc:/switch/KRKR-ns/` 目录下。升级时替换 NRO，保留已有游戏、存档和图片设置。
2. **真机**：使用完整内存模式打开 HBMenu，通常按住 `R` 启动一个已安装游戏，再加载 NRO；直接从相册进入的模式内存较小。入口按键可随 [Atmosphère 配置](https://github.com/Atmosphere-NX/Atmosphere/blob/master/config_templates/override_config.ini)改变。
3. **模拟器**：直接加载 NRO；下文的 SD 卡路径对应模拟器的虚拟 SD 卡目录。
4. 将游戏放到 `sdmc:/switch/KRKR-ns/Game/<GameFolder>/`，按上述规则命名，并保留资源之间的目录结构与脚本引用。在启动器选中游戏，按 `A` 开始；按 `X` 可选择启动用的 `.xp3`。同目录的其他 `.xp3` 会作为资源包挂载，下次启动会优先使用上次选择的入口。
5. 存档默认按游戏目录分开保存于 `sdmc:/switch/KRKR-ns/saves/<GameFolder>/`；同一游戏目录下的多个启动包共用该存档目录。
6. 自定义图片放到 `sdmc:/switch/KRKR-ns/Artwork/`。选中游戏后打开图片设置，分别为预览图和头像选择游戏图片或自定义图片，确认后会自动保存。

> **启动入口**：目前启动器需要 `.xp3` 入口，仅有展开的 `startup.tjs`、没有 `.xp3` 的游戏还不能直接从游戏库启动。
>
> **显示名称**：可在 `sdmc:/switch/KRKR-ns/Names.tjs` 中设置 `gameAliases["Game01"] = "中文显示名称";`，只改变游戏库中的名称。

> 诊断日志保存在 `sdmc:/switch/KRKR-ns/log/`，自动保留最近 3 份；运行时开关与源码补丁记录见 [PATCHES.md](docs/PATCHES.md)。
>
> 自行构建需准备 devkitPro / devkitA64、Switch 依赖库、CMake/Ninja 和 FFmpeg，并按本地环境配置脚本中的工具路径。FFmpeg 构建脚本见 [build_ffmpeg_switch.sh](tools/build_ffmpeg_switch.sh)；运行 `build_nro.sh` 生成 `build-switch/krkrsdl2.nro`，加 `--no-emu-copy` 可跳过模拟器复制。

## ✅ 已实现功能

> 兼容性差异与移植计划见 [KIRIKIROID2_COMPARISON.md](docs/KIRIKIROID2_COMPARISON.md) 和 [KIRIKIROID2_PORTING_PLAN.md](docs/KIRIKIROID2_PORTING_PLAN.md)。

**游戏库与界面**

- 浅色游戏库界面，支持手柄和触摸操作；A 确认、B 返回，支持翻页、启动文件选择和重新扫描。
- 自动选择并记住游戏的启动文件，支持自定义游戏显示名称。
- 浏览和切换游戏时不扫描游戏资源，点击「开始游戏」后再加载。
- 可从游戏图片或 SD 卡自定义图片中选择预览图和头像，支持分页浏览、自动裁剪和恢复默认；选择结果会保存，重启后仍可使用。
- 游戏结束后返回启动器，可继续选择其他游戏；启动内存不足时提供提示和重试入口。

**游戏运行与存档**

- 支持常见 KRKR / KAG 游戏，以及游戏目录中的多个资源包。
- 支持游戏设置、选项和确认弹窗，快速存档、读档可以正常处理确认操作；具体可用功能仍取决于游戏兼容性。
- 各游戏存档分开保存，连续快速存档时可正常轮换备份。
- 支持补丁和翻译资源优先加载，按目录查找资源，避免不同目录的同名图片被误用。
- 改善中文、日文脚本和字体兼容性；提供解密脚本的游戏，可尝试通过该脚本读取加密资源包。

**画面、动画与音视频**

- 支持背景、立绘、文字、选项和常见转场，按游戏画面比例显示。
- 支持 E-mote 动态立绘，改善有限动画结束后返回标题的流畅度。
- 支持 AlphaMovie 透明动画的播放、循环和跳帧。
- 内置原生 LayerExRaster 插件，支持游戏脚本调用的波纹图像效果，改善相关剧情转场的兼容性。
- 音乐与语音支持 WAV、OGG、Opus 等格式；视频支持部分 WMV、MP4、MPEG 格式，可从游戏目录或资源包播放，并支持音量调整。
- 内置文字绘制、脚本解析、存档和字体等常用插件；仍有部分插件与格式尚未支持。

**流畅度与稳定性**

- 缓存已显示的文字、图片和游戏资源，减少重复读取与绘制，改善游戏库切换和菜单响应。
- 检查图片读取和解码错误，无法预览的图片会显示提示，也可改用自定义图片。
- 修复部分影片播放中的音频断流，完善片尾音频处理和播放资源释放。
- 退出游戏时清理窗口、音频、定时器和事件，修复脚本清理异常引起的重复退出，改善返回游戏库的稳定性。
- 诊断日志自动保留最近 3 次。

## 🗂 架构

项目由启动器、引擎核心、Switch 平台适配、内置插件和脚本兼容层组成，各部分职责如下：

| 部分 | 主要职责 |
| --- | --- |
| 启动器 | `data/startup.tjs` 提供游戏列表、启动选择和预览图界面；`LauncherArtwork` 模块负责扫描图片、生成缩略图和保存选择。 |
| 引擎核心 | `external/krkrz/` 提供 TJS2 脚本执行、图层与位图、资源归档等公共逻辑；资源路径查找位于 `base/StorageIntf.cpp`。 |
| Switch 平台适配 | `src/core/` 对接窗口、输入、绘制、文件系统和音频；使用 SDL2 与 libnx 适配 Switch，FAudio 负责音频输出，FFmpeg 负责视频及视频音轨解码。 |
| 内置插件 | `src/plugins/` 提供 E-mote、AlphaMovie、LayerExRaster、文字绘制和脚本解析等功能，编译进 NRO，由引擎注册或按需启用；AlphaMovie 独立解码透明动画并输出到图层。 |
| 脚本兼容层 | `compat-patches/system/` 提供平台兼容脚本，随启动器和字体一起打包进 RomFS；SD 卡上的兼容补丁可覆盖内置版本。 |

启动器和游戏共用同一套引擎。浏览游戏库时只更新界面；开始游戏后才加载入口和资源包。游戏结束后重建引擎状态，再回到启动器。

仓库布局：

```text
KRKR-ns/
├── krkrsdl2/                       # 引擎源码
│   ├── external/
│   │   ├── krkrz/                  # KRKRZ 核心：脚本、图层、位图、资源归档
│   │   └── ...                     # SDL2、FAudio、simde、zlib 等第三方组件
│   ├── src/
│   │   ├── core/sdl2/              # 程序入口、画面呈现、日志、预览图处理
│   │   │   ├── SDLEntrypoint.cpp
│   │   │   ├── SDLApplication.cpp
│   │   │   └── LauncherArtwork*    # 图片扫描、解码、缩略图和设置保存
│   │   ├── core/base/sdl2/         # 文件与存储、脚本管理、插件加载
│   │   ├── core/visual/sdl2/       # 窗口、绘制设备、图层、视频播放
│   │   ├── core/sound/sdl2/        # 音频输出与解码
│   │   ├── core/environ/sdl2/      # 应用生命周期、事件、线程和系统信息
│   │   ├── core/msg/sdl2/          # 消息与对话框
│   │   ├── core/utils/sdl2/        # 剪贴板等平台工具
│   │   ├── plugins/               # E-mote、AlphaMovie、LayerExRaster、文字与存档等插件
│   │   ├── resources/nswitch/     # NRO 图标等平台资源
│   │   └── config/                # 构建使用的源码清单
│   ├── data/
│   │   ├── startup.tjs             # 游戏库启动器
│   │   ├── launcher/               # 启动器标志与默认预览图
│   │   └── notosanssc.ttf          # 内置中文字体
│   ├── CMakeLists.txt              # Switch 构建配置
│   └── meson.build                 # 上游构建配置
├── compat-patches/system/          # 打包进 RomFS 的 TJS 兼容脚本
├── design/launcher-preview/        # 启动器设计稿与预览文件
├── docs/                           # 兼容性、补丁和开发文档
├── tools/                          # 构建、打包和调试工具
├── tests/                          # 引擎、存档、动画等功能检查
├── out/                            # 本地 FFmpeg 依赖和发布 NRO，不纳入版本控制
└── build_nro.sh                    # 构建、NRO 打包与模拟器部署入口
```

> 文档索引：[docs/README.md](docs/README.md)；移植计划见 [KIRIKIROID2_PORTING_PLAN.md](docs/KIRIKIROID2_PORTING_PLAN.md)，源码补丁见 [PATCHES.md](docs/PATCHES.md)。[COMPAT_BACKLOG.md](docs/COMPAT_BACKLOG.md) 是较早的 krkrsdl3 对照记录；上游差异见 [UPSTREAM_DELTA.md](docs/UPSTREAM_DELTA.md)，由 `tools/upstream_delta.sh` 生成。

## 📄 许可

本项目自身代码沿用 [krkrsdl2 的 MIT 许可](LICENSE)。引用的上游与第三方组件（krkrz、FAudio、SDL2、FFmpeg、simde、zlib、FreeType、libjpeg-turbo、libpng、libogg/libvorbis、libopus 等）各自保留原始许可证与归属，见组件目录内的 LICENSE/COPYING 文件。

E-mote、AlphaMovie 与 LayerExRaster 的移植来源许可分别见 [E-mote 许可](krkrsdl2/src/plugins/emoteplayer/UPSTREAM-LICENSE.txt)、[AlphaMovie 许可](krkrsdl2/src/plugins/alphamovie/LICENSE.krkrsdl3) 和 [LayerExRaster 许可](krkrsdl2/src/plugins/layerexraster/LICENSE.krkrsdl3)；FFmpeg 的源码版本与构建选项见 [build_ffmpeg_switch.sh](tools/build_ffmpeg_switch.sh)。

**本项目不包含、也不分发任何商业游戏资源。**
