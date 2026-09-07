# KRKR Portable for Nintendo Switch — 移植任务规划

> 2026-09-05 范围纠正：目标是对照完整 KiriKiri/Kirikiri Z、Kirikiroid2 实现通用 NS 移植。krkrsdl2 仅作为原型和平台骨架，不作为功能完整性的验收标准。下面的旧版“已支持/零移植”结论不能作为完成证明；当前源码基线、缺口和验收边界见 [SOURCE_PORT_STATUS.md](SOURCE_PORT_STATUS.md)。保留旧规划供追溯。

## 1. 项目概述

### 1.1 目标

将 KiriKiri(吉里吉里 / KRKR)视觉小说引擎移植到 Nintendo Switch 自制系统(homebrew),使未加密 xp3 打包的 KRKR 游戏(尤其是中文/日文同人作品与汉化游戏)可以在 Switch 上以 `.nro` 形式运行。

**移植基础(已确认):[krkrsdl2](https://github.com/krkrsdl2/krkrsdl2)(吉里吉里SDL2)**

krkrsdl2 提供 SDL2 平台层和 `NINTENDO_SWITCH` 构建入口，但不能据此推定完整引擎和插件功能已经移植。项目需要逐模块核对完整项目源码：保留 TJS2、Layer、Storage 等语义，将平台接口实现为 Switch 后端，并以独立测试、原引擎对照、实际游戏和真机分层验收。

### 1.2 v1 支持 / 不支持清单

| 能力 | v1 状态 | 说明 |
|---|---|---|
| 未加密 xp3 归档 | ✅ 支持 | 覆盖大量同人与汉化作品,绕开密钥处理 |
| 展开的文件夹游戏(非打包) | ✅ 支持 | KRKR 原生支持目录形式 |
| TJS2 脚本 / KAG 框架 | ✅ 支持 | 纯 C++ 解释器 + TJS 脚本框架,跨平台代码,几乎零移植 |
| 图层合成 / 软件光栅 | ✅ 支持 | tvpgl.c 为跨平台共用代码 |
| 加密 xp3 | ❌ 不支持(v1) | 需密钥处理,且涉及具体游戏授权问题,列入 v2 |
| 内置视频播放(OP/ED) | ❌ 不支持(v1) | movie/win32 是 KRKR 移植死角(官方 Android 分支亦无),需 FFmpeg,列入 v2 |
| 第三方 DLL 插件 | ❌ 不支持(v1) | win32 DLL 无法在 Switch 运行,仅保留内置插件 |
| 存档 / 读档 | ✅ 支持 | SD 卡持久化目录 |

### 1.3 项目现状背景(调研结论,规划的前提事实)

- **无活跃的 KRKR Switch 移植先例**。唯一完整的尝试是 [uyjulian/krkrs](https://github.com/uyjulian/krkrs)(Kirikori for Nintendo Switch,SDL2 + libnx + FFmpeg,仅支持未加密 xp3),已于 2022-10 归档,作者宣布后续多平台开发转入 krkrsdl2。
- **krkrsdl2 的 Switch 目标存在已知问题**:
  - issue [#129 "Can'not run on Switch"](https://github.com/krkrsdl2/krkrsdl2/issues/129)(2025-06 提出,仍 open):按官方方式构建后在模拟器和真机上均崩溃,且长期无人处理。
  - 2023 年曾有可运行的 NRO 构建(issue #102/#104/#105,由 uyjulian 关闭),说明该目标**曾经能跑,后续回归**——这是有利线索,可通过 git 历史定位回归点。
  - 官方 release 页面从未发布 Switch 版 NRO。
- **Switch 上同类视觉小说引擎的既定范式**:ONScripter 家族([onsyuri](https://github.com/wetor/ONScripter-jh-Switch)、ONScripter-NX 等)全部采用 **devkitPro 工具链 + switch-sdl2 系列库 + Makefile/CMake + `.nro` + RomFS** 的组合,无一使用 deko3d。本移植照搬该范式。
- **KRKR 与 ONScripter 的本质差异**:ONScripter 天生跨平台,Switch 移植只是编译+修细节;KRKR 则把平台耦合写死在 win32(TVPWin32 窗口、GDI、DirectSound、DirectShow、IME、DLL 插件),移植是"逐层换血"。好在 krkrsdl2 已经完成了大部分换血工作。

## 2. 技术选型

| 项目 | 选择 | 说明 |
|---|---|---|
| 上游代码库 | krkrsdl2(submodule 固定版本) | 官方活跃维护;自身演进快,固定 commit 降低变量 |
| 工具链 | [devkitPro](https://devkitpro.org)(dkp-pacman) | Switch homebrew 标准工具链 |
| 依赖包 | switch-dev、switch-sdl2、switch-faudio、switch-libpng、switch-libjpeg-turbo、switch-libwebp、switch-freetype、switch-fribidi、switch-libass、switch-oniguruma、switch-libogg/libvorbis 等 | krkrsdl2 依赖清单,devkitPro 官方包基本全覆盖 |
| 图形 | switch-sdl2(GLES2 后端) | 上游已实现 SDL2 DrawDevice |
| 音频 | FAudio(switch-faudio) | krkrsdl2 已实现 `TVP_FAUDIO_IMPLEMENT`,所有非 Emscripten 平台统一走 FAudio |
| 构建 | CMake(复用上游 `NINTENDO_SWITCH` 目标)+ Makefile 打包 | 产出 `.nro` + RomFS |
| 运行载体 | Atmosphere + hbmenu,`.nro` 置于 SD 卡 | 开发阶段同时用 PC 模拟器(Ryujinx)加速调试 |
| 游戏加载 | v1 提供两种模式:① 开发/测试用 SD 卡目录加载+简单游戏选择器;② 发布用 RomFS 打包单游戏 | krkrs 已证明 RomFS 方案可行(代价:文件名须小写且仅 7-bit 安全) |
| 字体 | 内置 OFL 许可开源字体(思源黑体/Noto Sans CJK + 日文字形) | FreeType 渲染上游已支持;规避 GDI 字体依赖与版权问题 |

## 3. 架构分析:复用 vs 重写

基于对 krkrz 官方仓库(`dev_multi_platform` Android 分支)、krkrs 与 krkrsdl2 三层源码的结构性对比,得出移植工作量集中在以下模块:

### 3.1 可直接复用(跨平台代码,零移植)

- `tjs2/` — TJS2 脚本虚拟机(纯 C++ 解释器,无 JIT 需求,Switch aarch64 无压力)
- `xp3` 容器(未加密路径)与文件夹资源
- `visual/tvpgl.c` — 图层合成软件光栅核心
- `visual/FreeType*.cpp` — 字体渲染
- KAG 框架(本质是 TJS2 脚本,随 TJS2 原样运行)
- TLG / OGV / WAV 等格式解码

### 3.2 上游已完成的平台抽象(krkrsdl2 现有资产)

- SDL2 窗口 / 事件循环 / DrawDevice(替代 TVPWin32 + GDI)
- FAudio 音频后端(替代 DirectSound,含 WaveMixer 流式解码)
- 多平台目录结构(`src/resources/` 下已有 `nswitch` 资源目录)

### 3.3 本项目必须修复 / 新增的工作

| 模块 | 工作内容 | 参考依据 |
|---|---|---|
| Switch 构建目标修复 | 编译/链接错误清理;依赖包版本对齐 | issue #129 |
| 启动崩溃定位 | 对照 2023 年可运行版本的 git 历史做回归定位;模拟器日志二分 | issue #102-#105 曾有可用构建 |
| CPU 特性检测 | KRKR 存在 x86 CPUID / SSE 检测代码,需改为 aarch64 安全路径;tvpgl 的 SSE 汇编路径需确认 simde/NEON 覆盖 | krkrsdl2 已引入 simde |
| 手柄输入 | 左摇杆模拟鼠标指针、A/Z=左键、B/X=右键或菜单、L/R=快进/自动、+/−=系统菜单;触摸屏直控 | switch-sdl2 提供触控事件 |
| 文本输入 | KRKR 文本控件依赖 IME;v1 提供简单 ASCII 输入框(存档命名场景) | Android 版有 VirtualKey 先例 |
| 文件系统 | SD 卡游戏目录扫描与选择器;存档映射到 SDL_GetPrefPath(switch 上为 SD 卡路径) | krkrs 的 RomFS 方案对照 |
| 字体资源 | 内置 OFL 字体 + `Font.addFont` 注册(v1 需在启动脚本或代码中注册) | krkrs 需手动 `Font.addFont` |
| 插件 | 保留 krkrs 验证过的内置插件集(csvParser、dirlist、getSample、saveStruct、varfile、win32dialog 等 9 个) | krkrs 实测 |

## 4. 阶段规划与任务清单

每个阶段含验收标准;阶段间有依赖关系,建议顺序执行。

### Phase 0 — 环境准备(0.5–1 天)

任务:
- [ ] 安装 devkitPro(dkp-pacman 方式),安装 switch-dev 等基础包
- [ ] 部署 Switch 真机环境:Atmosphere + hbmenu(或确认已有)
- [ ] 安装/确认 PC 模拟器(Ryujinx,支持 homebrew .nro 调试)
- [ ] 编译官方 hello world 样例,验证 `.nro` 在模拟器与真机均可运行

验收:工具链全链路打通(源码 → .nro → 运行)。

### Phase 1 — 上游基线构建(1–2 天)

任务:
- [ ] clone krkrsdl2,按上游 README 构建 PC(Linux/macOS/Windows 任一)版本,验证上游本身可直接构建
- [ ] 配置 `-DNINTENDO_SWITCH=ON`,尝试 Switch 目标编译,完整记录所有编译/链接错误(预期复现 issue #129 前置症状)
- [ ] `git log` 定位 2023 年 Switch 目标可用时的 commit~现在之间的相关改动清单

验收:产出《Switch 目标错误清单》(编译错误逐条 + 涉及的依赖包 + 疑似回归范围)。

### Phase 2 — Switch 目标修复(3–5 天,核心阶段)

任务:
- [ ] 按错误清单逐一修复编译/链接问题(依赖包对齐、宏开关、缺失头文件)
- [ ] 处理 aarch64 架构相关问题(CPUID/SSE 代码路径,确认 simde 覆盖)
- [ ] 调试启动崩溃:模拟器 + 日志输出逐步定位(SDL 初始化 → TVP 系统初始化 → 图形/音频后端初始化,二分排查)
- [ ] 对照 git 历史中 2023 年可用版本的对应实现,优先回滚/修复回归点
- [ ] 加载最小测试资源,达到"游戏启动进入标题画面"状态

验收:**hello 级别的 KRKR 演示能启动到标题画面**,日志无致命错误。**此为 M1 里程碑。**

### Phase 3 — 输入与交互(2–3 天)

任务:
- [ ] 实现手柄→鼠标映射与按键映射表(见 3.3),含光标加速与灵敏度调参
- [ ] 实现触摸直控(点击=左键,长按=跳过)
- [ ] 实现文本输入框(v1:ASCII 存档命名)
- [ ] 在真实 KAG 流程中验证:标题→开始→对话推进→选项选择→存档/读档

验收:单手手柄可完整操作一个 galgame 的标准流程。

### Phase 4 — 文件系统与存档(1–2 天)

任务:
- [ ] 实现 SD 卡游戏目录扫描 + 简易游戏选择器(开发/测试模式)
- [ ] 实现 RomFS 打包模式(Makefile 集成,文件名小写化处理)
- [ ] 存档路径映射至 SD 卡持久目录,验证重启后存档保留
- [ ] 处理非 ASCII 路径/文件名(中文目录)的编码问题

验收:两种加载模式均可进游戏;存档/读档跨重启有效。**此为 M2 里程碑(可玩 MVP)。**

### Phase 5 — 图形与音频验证(2–3 天)

任务:
- [ ] 性能基准:1280×720 软件光栅图层合成在 Switch CPU 上的帧率实测
- [ ] 若不足 60fps:评估降分辨率 / NEON 优化(tvpgl 走 simde 路径)/ 局部硬加速(GLES 纹理上传)三条优化路线,优先做前两条
- [ ] FAudio 音频验证:BGM 循环无缝、波形播放、音量控制
- [ ] Switch 特殊形态验证:手持 / 底座 / 睡眠唤醒后恢复

验收:代表性游戏对话场景 ≥ 主流可玩帧率;BGM 循环正常;睡眠恢复无崩溃。

### Phase 6 — 兼容性测试(3–5 天)

任务:
- [ ] 采集测试集:公开、未加密 xp3 的 KRKR 演示/同人游戏(来源待确认和采集,需逐个验证许可)
- [ ] 按游戏逐项测试:启动、对话、立绘/背景切换、选项分支、存档读档、BGM/音效、Skip/Auto
- [ ] 真机回归:上述游戏在真机上完整跑通一局流程
- [ ] 缺陷跟踪与修复:记录每款游戏的兼容性清单(兼容度、已知问题)

验收:测试集中 ≥80% 游戏可完整通关流程;其余游戏有明确的已知问题清单。**此为 M3 里程碑(稳定版)。**

### Phase 7 — 打包与发布(1–2 天)

任务:
- [ ] 打包脚本:`.nro` + RomFS + 图标 + NACP 元数据
- [ ] 发布文档:安装说明、支持的游戏类型说明、已知限制(未加密 xp3 / 无视频)
- [ ] (可选)forwarder 生成桌面图标启动

验收:发布包可在全新 Atmosphere 环境一键安装运行。**此为 M4 里程碑(发布)。**

## 5. 里程碑与时间估算

| 里程碑 | 内容 | 累计工期(单人全职) |
|---|---|---|
| M1 | Switch 构建通过,演示进入标题画面 | ~1 周 |
| M2 | 可玩 MVP(输入 + 存档 + 双加载模式) | ~1.5–2 周 |
| M3 | 稳定版(兼容性测试通过) | ~3 周 |
| M4 | 发布包 | ~3–4 周 |

总估算:**单人全职约 3~4 周**,主要不确定性集中在 Phase 2(上游崩溃原因未知)与 Phase 6(测试集采集)。

## 6. 风险与对策

| # | 风险 | 等级 | 对策 |
|---|---|---|---|
| 1 | krkrsdl2 Switch 目标崩溃根因不明(issue #129 半年未修),可能涉及 SDL2/FAudio 初始化深层问题 | 高 | ① git 历史定位 2023 可用版回归点,优先回滚修复 ② 对照 krkrs(归档)的成熟实现移植其初始化代码 ③ 备选:绕过 krkrsdl2 的 Switch 分支,自写薄 SDL2 平台层(工作量 +1~2 周) |
| 2 | 软件光栅性能不足(720p 图层合成) | 中 | 降分辨率渲染、NEON/simde 路径、必要时 GLES 纹理上传(本地实测后决策) |
| 3 | 上游 krkrsdl2 快速演进导致反复跟进 | 中 | submodule 固定 commit;功能稳定后再定期同步 |
| 4 | FAudio 在 Switch 上的兼容性(初始化失败、无声音) | 中 | devkitPro 官方 `switch-faudio` 包;真机尽早验证,失败则退 switch-sdl2_mixer 自写后端 |
| 5 | 汉字/日文字体版权 | 低 | 内置 OFL 字体(思源/Noto);禁止打包商业字体 |
| 6 | 中文游戏目录/文件名编码问题 | 中 | RomFS 模式强制小写 7-bit 文件名;SD 卡模式优先用 UTF-8 规范路径(v1 已知限制:部分非 ASCII 路径可能异常) |
| 7 | 加密 xp3 涉及授权/法律边界 | 低 | v1 明确不支持并在发布文档声明;不对破解/密钥提取做技术说明 |
| 8 | 模拟器(Ryujinx)与真机行为差异(尤其 SDL2/FAudio 初始化) | 中 | 每阶段关键节点必须真机验证一次,不以模拟器结果代替 |

## 7. 测试策略

- **单元级**:沿用上游对 TJS2/格式解码的测试;移植改动集中于平台层,以集成测试为主。
- **集成级**:Phase 6 测试集,按游戏分级记录兼容清单(完全兼容 / 可玩但有瑕疵 / 不可运行+原因)。
- **执行方式**:开发期模拟器为主、每阶段末真机回归;发布前全量真机跑测试集。
- **测试集来源**:优先 krkrz 仓库自带的演示/样例与公开发行的免费演示版(需逐个确认许可,避免版权问题)。

## 8. 后续路线图(v2+)

1. 加密 xp3 支持(需密钥处理方案,依赖具体游戏授权)
2. 内置视频播放(移植 FFmpeg + FAudio 同步,参考 krkrs 已验证方案)
3. 插件体系扩充(按社区高频需求逐个重写内置插件)
4. 性能优化(NEON 手写内核、多线程图层合成、720p/1080p 选项)
5. 游戏管理增强(封面、分类、每游戏独立存档目录与配置)

## 9. 附录:参考资料

- krkrsdl2(移植基础):https://github.com/krkrsdl2/krkrsdl2
- Switch 目标崩溃 issue #129:https://github.com/krkrsdl2/krkrsdl2/issues/129
- krkrs(已归档的 Switch 专用移植,重要参考):https://github.com/uyjulian/krkrs
- krkrz(官方吉里吉里Z,dev_multi_platform 分支为 Android 平台层参考):https://github.com/krkrz/krkrz
- Kirikiroid2(移动端最成功的 KRKR 移植,平台层重写范式参考):https://github.com/zeas2/Kirikiroid2
- onsyuri(ONScripter Switch 移植,工程范式参考):https://github.com/wetor/ONScripter-jh-Switch
- devkitPro(工具链):https://devkitpro.org
