# KRKR-ns

吉里吉里 (KiriKiri / KRKR) 视觉小说引擎的 Nintendo Switch 自制 (homebrew) 移植，基于 [krkrsdl2](https://github.com/krkrsdl2/krkrsdl2)（pinned `bf207f2`），内嵌引擎为 krkrsdl2/krkrz（pinned `b11c43a`）。

播放未加密 xp3 打包的 KRKR/KAG 游戏；也支持展开的目录式游戏。附带一个简单的游戏浏览器启动器（`sdmc:/krkr/<游戏目录>/`）。

## 使用

- 用 devkitPro 工具链构建（`NINTENDO_SWITCH=ON`），产物为 `build-switch/krkrsdl2.nro`；`build_nro.sh` 打包全部流程。
- 游戏放置：`sdmc:/krkr/<游戏目录>/`，启动器列出其中的 .xp3（同一目录的所有 .xp3 自动挂载，`启动游戏.xp3` 默认选中）。
- 存档：`sdmc:/switch/krkrsdl2/saves/<游戏目录>/`。
- 真机以 hbmenu 运行（按住 R 进 title-override 等方式加载）;开发期可用 PC 模拟器（Ryujinx fork，如 Nextendo）调试。

## 运行时标记文件（sdmc:/switch/krkrsdl2/）

| 文件 | 作用 |
|---|---|
| `emote-cpu.txt` | 强制 E-mote CPU 后端（模拟器软件 GLES 坏驱动默认会在此自动回退） |
| `emote-gl.txt` | 强制 E-mote GL 后端 |
| `emote-halfres.txt` | E-mote 半分辨率渲染 |
| `tvpgl-scalar.txt` | 关闭 simde SIMD 合成内核（标量回退，A/B 用） |
| `dirtyrect-update.txt` | 脏矩形纹理上传（实验性） |
| `trace-render.once` | 一次性渲染追踪（BMP 快照 + 图层树 dump） |
| `platform-windows.txt` | 伪装 TVPGetPlatformName 为 "Windows" |

## 本仓库的本地修改

全部补丁清单见 [PATCHES.md](PATCHES.md)；开发复盘见 [DEVLOG.md](DEVLOG.md)；性能优化方案与实测数据见 [OPTIMIZATION_PLAN.md](OPTIMIZATION_PLAN.md)。

主要方向：

- Switch 显示链修复（SDL 渲染器 + 1280×720 逻辑分辨率 + 纹理/表面创建时序）
- 游戏加载模式（`sdmc:/krkr/` 多游戏浏览器 + xp3 自动挂载 + 补丁目录覆盖）
- 手柄→鼠标/键盘事件合成（NS 物理键位语义）
- KAG 兼容层（`compat-patches/`，romfs 内置 + SD 卡可覆盖）
- 性能：simde SIMD 混合内核激活（NEON）、E-mote 默认 GL 后端 + 驱动自检探针、绘制线程池（4 核）、XP3 段缓存扩容
- 诊断基建：统一 SD 日志 `sdmc:/krkrsdl2_debug.log`、每帧分段剖析 `[prof]`、心跳/阶段标记

## 许可

本仓库以 [krkrsdl2 的 MIT 许可](LICENSE) 发布；内嵌/引用的上游组件（krkrz 引擎、FAudio、SDL2 及其 Switch 端口、simde、zlib、FreeType、libjpeg-turbo、libpng、libogg/libvorbis、libopus 等）各自保留其原始许可证与归属，见各组件目录内的 LICENSE/COPYING 文件。**不包含任何商业游戏资源**。