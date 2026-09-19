# KRKR-ns 与 Kirikiroid2 架构与代码差异对照

> 盘点：2026-09-19。对照对象：**zeas2/Kirikiroid2**（公有源码 `d1c2b12`，本地 `.zcode/upstream-kirikiroid2` 或 `project/Kirikiri-ns/kirikiroid2`）。
> 目的：回答「Kirikiroid2 能跑而我们不能（或行为不一致）」的标题到底差在哪些架构与代码点上。
> 方法：目录/规模盘点、原生类成员逐项 diff（Window/Layer）、插件清单对照、以及 2026-09 会话中全部实机故障的根因回溯。
> 注意：Kirikiroid2 的 APK 里还有一批**未公开源码的私有内置插件**（社区俗称「私有插件仓库」），公有树里看不到；本文件对这部分只能列「已知存在」清单。

---

## 1. 工程定位总览

| | **KRKR-ns（本项目）** | **Kirikiroid2** |
|---|---|---|
| 基底 | krkrsdl2（krkrz 引擎 + SDL2 平台层） | krkr2/krkrz 血统 + **cocos2d-x** 渲染/窗口 |
| 目标平台 | Nintendo Switch（Horizon + homebrew） | Android（APK 分发），代码保留 win32/linux/sdl 分支 |
| 显示合成 | **CPU 合成**（krkrz software blender）+ GL 呈现补丁（glc/P60 系列） | **GPU 合成**：图层 = cocos2d 精灵/纹理，混合走着色器 |
| 内置插件策略 | 静态注册表 + `TVPGetPlacedPath` 对内置名返回合成路径 | `TVPLoadPlugin` 直接 `return;`（seal 全部外部 DLL），仅内部插件可达 |
| 游戏适配哲学 | 逐故障修复 + 平台垫片（compat-patches） | 私有插件仓库让「能力探测」全部通过 + 游戏 TJS 自降级 |

两条线同宗（krkr2/krkrz 的 TJS2 与核心语义一致），**所有「同一段游戏 TJS 两边行为不同」的问题，差异都在原生成员的实现或缺失上**，脚本层本身没有分叉。

---

## 2. 代码规模与结构对照

| 子系统 | Kirikiroid2 | KRKR-ns | 差异要点 |
|---|---|---|---|
| core/base | 24.4k 行 | krkrz/base 35.7k + base/sdl2 16.8k | 我们以 krkrz 为基底多了 SDL 平台实现；K2 精简过 win32 依赖 |
| core/visual | 91.6k 行（含 gl/ ARM/） | krkrz/visual 86.2k + visual/sdl2 11.2k | K2 有 ARM NEON 汇编优化与 gl 子目录；我们用 simde SSE 模拟 |
| 窗口/显示环境 | environ/cocos2d 3.6k（MainScene 2448 行） | src/core/sdl2 10.9k（SDLApplication） | 完全不同的显示框架 |
| 插件 | plugins 3.7k（公有 13 个） | src/plugins 19.3k（17 个内置） | 我们内置了 E-mote/PSB 等 K2 放在私有仓库的东西 |
| 脚本引擎 | src/core/tjs2（同源 krkrz TJS2） | external/krkrz/tjs2 | 同源；K2 多了 PhaseVocoder 音频 DSP 与 Debug 基建 |
| KAGParser | core/utils/KAGParser.cpp（内置） | src/plugins/kagparser（内置） | 两边都内置 |

---

## 3. 逐子系统差异

### 3.1 显示与窗口

| 项 | Kirikiroid2 | KRKR-ns | 影响 |
|---|---|---|---|
| 渲染框架 | cocos2d-x：图层树直接映射为 CCLayer/纹理，GPU 混合 | krkrz CPU 合成管线 + SDL texture 呈现 | 性能与画质上限；我们 CPU 合成是帧率瓶颈（compose 13-102ms/f） |
| 内容几何 | `MainScene` 声明式：游戏 `SetSize/SetInnerSize` → `RecalcPaintBox()` 等比缩放进视图 | `krkrsdl2_present_crops()` 启发式裁剪 + P87 `declaredClientW/H`（仿 K2 补的） | 「窗口大小不对/画面缺一块」类故障的历史根源；K2 的方案是原始参照 |
| 全屏语义 | `GetFullScreenMode()` 恒 false（窗口层） | 曾恒 true（引发 LimeLight 白屏），后按 K2 改回真实语义 | 已对齐 |
| 原生 Window 类成员 | 73 个 | 79 个（超集：另含 canvas/drawCycle/exSystemMenu/mouseCursor/displayDensity/fireOnDraw） | 我们是超集，无缺口 |
| 多点触摸/旋转 | onMultiTouch/onTouchScaling/onDisplayRotate 完整 | 成员存在，Switch 上无触摸硬件 | 无实际影响 |
| 多窗口 | 主窗口 + 模态子窗口共用单 GL surface | P57-58 时代实现「寄宿窗口」 | 已对齐（KAG 模态对话框可用） |

### 3.2 文本渲染与 TextRender（历史记录 bug 的根因子系统）

| 项 | Kirikiroid2 | KRKR-ns |
|---|---|---|
| 基础文字绘制 | `LayerImpl.drawText` 原生实现 + `FontSystem`/`FreeTypeFontRasterizer`/`PrerenderedFont`（字体图集缓存） | `FreeTypeFontRasterizer`（krkrsdl3 同源）+ `PagedCharacterCache`（自研） |
| TextRender 插件 | **APK 私有内置**（公有树无源码）；导出原生 `TextRenderBase` 类，`textrender.tjs` 代理成 `TextRender` | krkrsdl3 移植版（`src/plugins/textrender/TextRenderBase.cpp`），方法集一致（render/setRenderSize/getCharacters/calcShowCount/renderText 等属性） |
| 语义差异（历史记录 bug 根因） | 真机 DLL 的 `renderText` 在「渲染后立即读」即返回本次渲染文本；且 KAGEX 的 `HistoryTextStore.storeRender(prev, new)` 依赖该时序 | 初版按 krkrsdl3「flush 时累加」实现 → 游戏读到空串 → 历史条目只有名字无正文；已改 push 时累加，最终增量语义仍与真机有出入（2026-09-19 未完） |
| KAGEX 兼容面 | 私有插件让 KAGEX 的能力探测（CanLoadPlugin）全部通过，游戏走 TextRender 全功能路径 | 我们的插件探测（`TVPGetPlacedPath` miss 分支 + 内置名单）让探测通过，但语义深度不足的地方（如本例）就会静默降级或出错 |

**结论**：TextRender 是 K2 私有插件里最核心、公开资料最少的一个。历史记录 bug 的最终修复必须以 APK 内 `libkirikiroid2.so` 的实现为准（或用运行时探针逐字段确认条目数据结构）。

### 3.3 插件体系对照

| 插件 | Kirikiroid2 公有 | K2 APK 私有（社区 DLL list/逆向已知） | KRKR-ns 内置 | 状态 |
|---|---|---|---|---|
| xp3filter | ✅ | — | ✅（XP3ExtractionFilter + 原生 xor 快路径） | 对齐 |
| win32dialog | ✅（去 UI 版） | — | ✅（TJS 垫片+原生混合） | 对齐 |
| varfile / csvParser / dirlist / addFont / fftgraph / getSample / getabout / saveStruct | ✅ | — | ✅ | 对齐 |
| wutcwf | ✅ | — | ✅ | 对齐 |
| layerExMovie / layerExPerspective | ✅ | — | ❌（backlog P2） | 缺 |
| **textrender** | ❌（源码不在公有树） | ✅ APK 内置 | ⚠️ 自研移植（语义有差距） | **历史记录 bug 所在** |
| **windowEx** | ❌ | ✅（APK） | ❌（自造面已按 P86 收回） | KAGEX windowEx 调用靠 KAGEX 自带兜底 |
| **LayerExImage / layerExDraw / layerExRaster** | ❌ | ✅（APK） | ⚠️ LayerExImageCompat.tjs 垫片（clipAlphaRect 等） | xgkfg 故障源；draw/raster 缺（backlog P1/P2） |
| **extrans** | ❌ | ✅（APK） | ❌ | 转场退化 crossfade（backlog P1） |
| **AlphaMovie** | ❌ | ✅（APK） | ⚠️ 桩（TJS model-only） | ここは… 走 1 秒空影片 |
| json / fstat / sqlite3 / PackinOne / lzfs | ❌ | ✅（APK） | ❌（fstat 部分有） | backlog P2 |
| **emoteplayer（E-mote）** | ❌（K2 靠 APK 私有仓库） | ✅（APK） | ✅ **自研完整移植**（开源 GL 后端） | 我们领先 |
| **psbfile** | ❌ | ✅（APK?） | ✅ 自研 | 我们领先 |
| kagparser | ✅（utils 内置） | — | ✅ 内置 | 对齐 |
| kremscripten / layerexbtoa（GFX_Motion） | ❌ | ? | ✅ | 我们独有 |

### 3.4 脚本引擎与 KAG

- 同源 krkrz TJS2。K2 的多线程改造（variant 栈移交引擎、`tTJSInterCodeContext` 成员化）我们已在 P60 系列等价落地（thread_local 栈）。
- K2 源码自带 `tjsDisassemble.cpp`（官方反汇编器，操作数=×16 字节偏移、CALLD 变长带参数对、vdata=(type,index) 表）——2026-09-19 已据此写出正确的静态解码器，取代了此前错位的 disasm_tjs.py 解读。
- KAGParser：双方都内置。K2 在 `core/utils`，我们在 plugins/kagparser。
- 字节码兼容：两边加载同一游戏的编译 TJS（TJS210 格式）均正常。

### 3.5 归档与加密

- XP3：对齐（含内容过滤器钩子）。
- xp3filter：对齐（K2 plugins/xp3filter.cpp ↔ 我们 XP3ExtractionFilter.cpp，含原生 xor 快路径）。
- K2 多 ZIP/TAR/Repack；我们多 7z（按内容嗅探）。
- xp3 内 7z：我们支持（P33 时代），K2 无。

### 3.6 输入

- K2：触摸为主（onTouch* 完整）、Android 手柄。
- 我们：Switch 手柄 → SDL 光标/按键合成（joy-axis warp、十字键=方向键、点击延迟 20ms 修正）+ 触摸（真机）。
- 差异无功能缺口，但「点击判定读 layer 光标」类游戏的时序问题需要我们的 20ms 延迟补丁（P80）。

### 3.7 影音

- K2：`core/movie/ffmpeg` 子集 + `krmovie.cpp`，cocos2d YUVSprite 显示。
- 我们：`SwitchMovieOverlay`（FFmpeg 解码 + FAudio 混音，MPEG-PS 补丁）。
- 都不支持 AlphaMovie（我们用 TJS 桩让剧情不卡）。

### 3.8 音频

- 同源 krkrz WaveIntf；K2 多 FFWaveDecoder（FFmpeg 音频，覆盖 mp3/aac/wma）+ PhaseVocoderDSP（变速不变调）。
- 我们：FAudio + Vorbis/Opus 内置；mp3/aac 仍未接（backlog P0）。
- K2 有 ARM 音频算法（src/core/sound/ARM）。

### 3.9 性能相关

| 项 | K2 | 我们 |
|---|---|---|
| 图层合成 | GPU（cocos2d 着色器） | CPU（krkrz blender）+ GL 呈现 |
| 像素算法 | ARM NEON 汇编（tvpgl_arm） | simde SSE 模拟（无 NEON） |
| 字体 | PrerenderedFont 图集 | PagedCharacterCache（自研，等价方向） |
| 音频算法 | ARM 版 | C 通用版 |

### 3.10 KRKR-ns 独有（K2 没有的）

- Switch 平台层本身：homebrew 启动器（游戏库/封面/记忆入口）、MTP 日志治理、真机 NRO 构建。
- 进程内整引擎重建（连续换游戏，P60 系列）。
- SwitchMovieOverlay：FFmpeg 影片解码 + FAudio 音轨（K2 的影片走 cocos2d YUVSprite + ffmpeg 子集，功能等价但实现不同）。
- E-mote 开源 GL 后端（K2 的 E-mote 是私有插件）。
- PSB 容器解析（psbfile 内置）。
- 手柄/触摸合成输入层 + 兼容垫片体系（compat-patches：k2compat、GFX_Motion、LayerExImageCompat、win32dialog 等）。
- 大量运行时容错（脚本错误继续、[eval] 风暴哨兵、存档隔离、auto-path 增量构建等）。

---

## 4. 已知游戏故障 ↔ 差距映射（2026-09 会话汇总）

| 故障 | 标题 | 差距点 | 状态 |
|---|---|---|---|
| 历史记录（履历）无文本 | 永不枯萎（KAGEX+TextRender） | TextRenderBase 的 `renderText` 时序/跨实例语义 ≠ 真机 DLL；`storeRender(prev,new)` 增量链断裂 | **未修**（机制已完全查明，待真机语义参照） |
| 开机白屏 | LimeLight | `Window.fullScreen` 语义 | 已修（对齐 K2） |
| 窗口大小/画面裁剪 | 多个 KAGEX | 内容几何声明（K2 MainScene RecalcPaintBox 模式） | 已修（P87 对齐） |
| 菜单闪退 | ここは… | MenuItem 类 | 已修（TJS 模型/原生按需） |
| E-mote 不渲染 | 多个 | 私有插件探测 | 已修（自研内置） |
| 转场全炸 ×4067 | LimeLight | sysTransitionEffect/layerStwCopy（K2 APK 私有） | 未修（backlog P1） |
| clipAlphaRect 缺失 | xgkfg | LayerExImage（K2 APK 私有） | 垫片已修 |
| 影片后闪退 | 晴菜花 | SwitchMovieOverlay 音频环形缓冲越界 | 已修 |
| 存档删除失败 | 晴菜花 | 路径规范化 + TJS 层包装 | 已修 |

---

## 5. 差距修复建议（按收益排序）

1. **TextRender 真机语义**（历史记录、以及一切 KAGEX+TextRender 标题的文本细节）：拿 Kirikiroid2 APK 提取 `libkirikiroid2.so`，逆向其 TextRenderBase 的 renderText/storeRender 协作语义；或运行时探针逐字段确认 HistoryTextStore 的条目结构。
2. **extrans 转场**（krkrsdl3 有 9k 行参考实现）：魔女的夜宴等已命中。
3. **LayerExDraw/layerExRaster**：自绘 UI 标题。
4. **FFmpeg 音频解码接入 WaveIntf**（FFmpeg 已随影片路径集成，纯接线工作）。
5. **Layer 缺失的 4 个成员**：affineBlend/blendRect/stretchBlend/stretchPile（K2 LayerIntf 有，我们没有；krkrsdl3 同样没有，需从 K2 移植）。
6. **ARM NEON**：把 K2 的 tvpgl_arm 移植过来替换 simde 路径（纯性能，风险低）。

---

## 6. 同名文件函数级扫描（2026-09-19 补充）

> 背景：缺字 bug（九次九日九重色）暴露了本报告 v1 的方法论盲区——只对比了「有什么」（类/成员/插件清单），没有对比**同名文件内部的实现差异**。`FreeTypeFontRasterizer.cpp` 两边同名同接口，但 K2 版多了 `ApplyFallbackFace` 字形回退（约 60 行），我们的 krkrsdl3 血统版本没有 → 中文文本在日文内嵌字体下缺字。已照 K2 源码移植修复（构建 886e35b8）。
> 本节 = 对两棵树**全部同名 .cpp** 做函数名级扫描的结论（K2 有、我们没有的函数），剔除平台噪音（Direct3D/Win32 注册表/MF 视频/启动器专属）后的**真实行为缺口**：

| 文件 | K2 独有函数 | 性质 | 建议 |
|---|---|---|---|
| tjsString.cpp | `IndexOf` `SubString` **`Trim`** | **TJS String 原生方法**——脚本直接调用，缺失即脚本异常 | 高优：照 K2 补齐 |
| tjsVariantString.cpp | `GetLength` | TJS String 内部方法 | 同上 |
| LayerIntf.cpp | `AffineBlend` `BlendRect` `StretchBlend` `AssignTexture` `StretchPile` `DoUserFontSelect` `InternalComplete2_GPU` `InternalDrawNoCache_CPU` | Layer 原生成员（图像合成/字体选择） | 高优（合成类）；DoUserFontSelect 与字体选择相关 |
| LayerBitmapImpl.cpp | `AssignTexture` `InternalBlendText` `IsIndependent` `IsOpaque` | 位图纹理操作 | 中优 |
| LayerManager.cpp | `CopyRect` `GetOrCreateDrawBuffer` `SetHoldAlpha` | 合成管线 API | 中优（与 GPU 合成改造相关） |
| XP3Archive.cpp | `Create` `Init` `TVPSetXP3ArchiveContentFilter` | **TJS 可见的 XP3 归档对象 + content filter**（新版加密方案） | 高优（新型加密标题） |
| TextStream.cpp | `TVPStringEncode` `_TextStream_mbstowcs` | 文本编码转换 | 中优（编码类故障相关） |
| VorbisWaveDecoder.cpp | `Render` `SetPosition` `SetStream` | Vorbis 解码器 seek/位置支持 | 低优 |
| WaveFormatConverter.cpp | `PCMConvertLoopFloat32ToInt16_c` `PCMConvertLoopInt16ToFloat32_c` | float32 PCM 转换 | 低优 |
| CharacterData.cpp | `Alloc` | 字形数据分配优化 | 低优 |
| EventImpl.cpp / TVPTimer.cpp | `tTVPContinuousHandlerCallLimitThread` 系列 | 连续事件调用限频线程 | 我们有自研等价物（P60），实现方式不同 |
| tjsLex/tjsCompileControl/tjsDateParser | `TJS_iswalpha` 等自定 wchar 分类 | locale 无关的字符分类 | 低优（我们用平台 libc） |

> 剩余同名文件的 K2 独有函数（Application/Platform/SystemImpl/WindowImpl 的 Direct3D、注册表、Win32 消息、MediaFoundation 系）为平台差异，不适用。
> **方法教训**：同名同接口文件必须做函数名级 diff——本次缺字 bug 的 `ApplyFallbackFace` 在这个扫描里会直接以「K2 独有函数」现身。该扫描脚本应作为每次同步上游后的常规步骤。
