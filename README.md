# KRKR-ns

吉里吉里 (KiriKiri / KRKR) 视觉小说引擎的 Nintendo Switch 自制 (homebrew) 移植，基于 [krkrsdl2](https://github.com/krkrsdl2/krkrsdl2)（pinned `bf207f2`），内嵌引擎为 krkrsdl2/krkrz（pinned `b11c43a`）。

播放未加密 xp3 打包的 KRKR/KAG 游戏；也支持展开的目录式游戏。附带一个简单的游戏浏览器启动器（`sdmc:/krkr/<游戏目录>/`）。

## 下载

无需自行构建：到 [Releases](https://github.com/DrFlyingPig/krkr-ns/releases) 下载最新 `krkrsdl2.nro`，放到 SD 卡 `sdmc:/switch/` 下即可（真机经 hbmenu 加载，需按住 R 进 title-override；模拟器直接加载该 nro）。

## 使用

- 自行构建：devkitPro 工具链（`NINTENDO_SWITCH=ON`），产物为 `build-switch/krkrsdl2.nro`；`build_nro.sh` 打包全部流程。
- 游戏放置：`sdmc:/krkr/<游戏目录>/`，启动器列出其中的 .xp3（同一目录的所有 .xp3 自动挂载，`启动游戏.xp3` 默认选中）。
- 存档：`sdmc:/switch/krkrsdl2/saves/<游戏目录>/`。
- **游戏内「结束游戏」返回内置启动器**，可直接继续启动下一个游戏（真机经 envSetNextLoad 链式重启 NRO，模拟器等同进程重建引擎；详见 [PATCHES.md](PATCHES.md) P60-R2）。
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
| `capture-once.txt` | 内容=秒数；进程启动该秒数后截取一帧（surface/present BMP + 图层树），自毁 |
| `platform-windows.txt` | 伪装 TVPGetPlatformName 为 "Windows" |
| `autocycle.txt` | **仅测试用**：游戏约 20 秒自动结束、启动器轮流启动各游戏目录（4 轮后自毁）；存在期间启动器行为与正式版不同 |

## 本仓库的本地修改

全部补丁清单见 [PATCHES.md](PATCHES.md)；开发复盘见 [DEVLOG.md](DEVLOG.md)；性能优化方案与实测数据见 [OPTIMIZATION_PLAN.md](OPTIMIZATION_PLAN.md)。

主要方向：

- Switch 显示链修复（SDL 渲染器 + 1280×720 逻辑分辨率 + 纹理/表面创建时序）
- 游戏加载模式（`sdmc:/krkr/` 多游戏浏览器 + xp3 自动挂载 + 补丁目录覆盖）
- **游戏内退出回启动器、连续更换游戏**（模拟器同进程整引擎重建 + 全套跨会话状态清理，见 P60-R2）
- 手柄→鼠标/键盘事件合成（NS 物理键位语义）
- KAG 兼容层（`compat-patches/`，romfs 内置 + SD 卡可覆盖）
- 性能：simde SIMD 混合内核激活（NEON）、E-mote 默认 GL 后端 + 驱动自检探针、绘制线程池（4 核）、XP3 段缓存扩容、增量 auto-path 索引、解码图像缓存
- 诊断基建：统一 SD 日志 `sdmc:/krkrsdl2_debug.log`、每帧分段剖析 `[prof]`、心跳/阶段标记、资源未解析探针 `[miss]`

## 许可

本仓库以 [krkrsdl2 的 MIT 许可](LICENSE) 发布；内嵌/引用的上游组件（krkrz 引擎、FAudio、SDL2 及其 Switch 端口、simde、zlib、FreeType、libjpeg-turbo、libpng、libogg/libvorbis、libopus 等）各自保留其原始许可证与归属，见各组件目录内的 LICENSE/COPYING 文件。**不包含任何商业游戏资源**。