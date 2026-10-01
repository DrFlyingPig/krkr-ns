# Kirikiroid2 兼容性与移植计划

更新：2026-10-01。本文件合并架构对照、空占位审计与旧兼容性 backlog，统一列出当前能力和实施顺序。故障过程与性能证据见 [RUNTIME_NOTES.md](RUNTIME_NOTES.md)，模块契约见 [MODULES.md](MODULES.md)，源码改动见 [PATCHES.md](PATCHES.md)。历史审计保留日期与参考版本，不沿用已经过时的缺失标记。

## 当前状态与验收边界

- 项目采用 krkrz 核心与 SDL2/libnx 平台层；游戏脚本、原生 API、回调、对象寿命和图层像素是兼容目标。Switch 输入、文件系统、启动器和静态插件加载在平台边界适配。
- 2026-10-01 12:50 正式包包含软件绘制优化、FFmpeg 波形后备解码、未完成脚本构造的 Window 清理修复，以及线程池失败回退和视频资源释放。用户已复测确认人物语音、快速读档、菜单与剧情正常。包身份及对应日志见 [运行记录](RUNTIME_NOTES.md#current-build)。
- 用户在 9 月 30 日确认过小幅改善，之后仍反馈帧率低。现有证据不能给出固定或普遍的 FPS 提升；主机局部耗时、模拟器窗口更新频率与实际流畅度分别记录。
- 已有基线真机反馈不能替代本轮新增菜单、视频控制、语音与绘制路径的物理 Switch 专项验收。启动器、标题画面、无崩溃和完整 NRO 链接均只是检查点。
- **Switch 真机的游戏路径不能包含中文。** 单个存储接口的 Unicode 测试结果，不代表完整真机启动与归档路径链路支持中文目录。

## 固定来源与实施原则

| 来源 | 固定参考 | 用途 |
|---|---|---|
| [Kirikiroid2 公有源码](https://github.com/zeas2/Kirikiroid2/tree/d1c2b1259423542c893e0b65eaeb46c848848f2b) | `d1c2b1259423542c893e0b65eaeb46c848848f2b` | 平台无关行为、RenderManager、XP3、音频与静态插件契约 |
| krkrz/krkrz | `fd5c4baa6a2ef5978db1bd043634351f48667daf` | Layer/DrawDevice、位图与原引擎语义 |
| krkrz/krkrz_dev | `472482fc0bd2f57d95d260e6970e847b3e8cf38d` | 完整发行工程及标准插件/测试清单 |
| krkrsdl2/krkrsdl2 | `bf207f27d683834d35146736ec8d83d397ec75a2` | 项目平台原型，当前工作树含后续修改 |
| krkrsdl2/krkrz | `b11c43a07543756fcb943a60ff0a949a7f470f10` | 项目内嵌引擎基线 |
| krkrsdl3/krkrsdl3 | `c014d307b02a2ee8f10b69b39ba8c31e8ee254d4`，部分模块另记文件提交 | 可公开核对的插件参考，不替代 K2 主线契约 |
| 2468785842/krkr2 | `dca7264572a63e753c1b07ca7053513d1751ce70` | motionplayer 等补充参考 |

标准 `krkrz/test_scripts` 参考 `8ca5cf18b7d5a70d0af12da713fe8de029e7cb95`，KAGParser 参考 `b26f47607b65ef8e69a88ff085709500f81b3224`。本地参考工程保留用于 diff；移植文件保留各自版权及许可证，不能把整个派生工程统一视为 MIT。

先核对签名、默认参数、错误、回调、状态和所有权，再迁移实现。方法名、插件加载或 `typeof` 探测成功不能证明语义完整。Switch 的 HWND、Android/cocos UI、文件和输入差异在平台边界处理，不按游戏名修改核心行为或覆盖游戏原来的能力选择。

静态插件只登记实际模块；Windows DLL 不在 Switch 执行，忽略 DLL 名称不代表实现了它。分别记录源码、主机测试、Switch 完整链接、模拟器专项、实际游戏操作与真机证据，未完成的层级保持未验收。

## 架构与当前能力

| 子系统 | Kirikiroid2 公开路径 | KRKR-ns 当前路径及边界 |
|---|---|---|
| 图层与呈现 | RenderManager 可选 software/OpenGL，公有源码默认 `software`；GL 分支用纹理/FBO/着色器合成，完成纹理交给 cocos Sprite | krkrz Layer 主要在 CPU 位图完成，再交给 SDL；GL 呈现模式不是完整 GPU Layer 移植。SIMDe/NEON 与行带并行已启用 |
| 窗口与几何 | 声明尺寸经 MainScene/RecalcPaintBox 等比映射 | SDL 共享显示表面、声明客户区和寄宿窗口适配；fullscreen 使用真实窗口语义。成员数量多不证明全部实现 |
| TJS2/KAG | 同源核心，KAGParser 内置 | 核心在编译执行，部分线程栈使用 thread_local；标准测试未全部验收，同源或编译脚本能运行不证明 VM 完全等价 |
| 字体/文本 | FreeType/PrerenderedFont；公有树没有完整私有 TextRender | PagedCharacterCache、字形 fallback、FreeType 会话清理已有实现；TextRender 排版和增量历史条目仍需逐字段核对 |
| 归档/过滤 | XP3、ZIP/TAR、content/extraction filter | XP3、7z 与内容探测已有实现；FileName/context/整文件及六参数过滤契约已验收，ZIP/TAR 未接当前 creator 链 |
| 独立音频 | FFWaveDecoder、专用格式与 PhaseVocoderDSP | FAudio；Opus → Vorbis → RIFF → FFmpeg。AAC/M4A-in-.ogg 按内容识别，格式范围取决于静态 FFmpeg；变速不变调 DSP 未移植 |
| 影片 | FFmpeg/KRMoviePlayer、平台显示分离 | SwitchMovieOverlay 自定义 AVIO、视频/FAudio；音频补给/EOS、position/frame、分段循环已有路径；VFR 精确帧定位、倍率、多轨仍有边界 |
| AlphaMovie | 公有树有相关 ARM IDCT 片段，完整私有插件不可直接核对 | 原生 AJPM mode 1/2、受限缓存、Layer 输出、下一段与生命周期；来源及 DLL 差异见 [模块说明](MODULES.md#alphamovie) |
| 菜单 | 公共 MenuItem API 与平台 UI | 脚本与 opt-in native 树保留类型边界，共用 Layer popup；每窗口根、排序/单选/快捷键/输入隔离及失效清理已有实现 |
| 输入/系统 UI | Android/cocos 触摸、手柄、对话框 | Switch 手柄/触摸经 SDL 合成事件；轮询式触摸、Pad、完整输入框/模板对话框仍需实现 |
| E-mote/私有插件 | 公有 K2 没有完整插件源码 | 有实际动画与 CPU/GL 后端；颜色、timeline、同步/物理等控制不完整，不能由插件名推导 APK 等价能力 |

K2 的公开 GPU 结构可以指导移植，不能据公有树缺插件推断 APK 缺功能，也不能臆测私有 E-mote 的缓存/回读。software 模式同样上传 CPU 像素。GPU 迁移需贯通纹理所有权、混合、蒙版、裁剪与更新区域，只改最终上传或开启标记不具备同等能力。

## 已完成的通用契约

### XP3 过滤与 Layer 绘图：2026-09-21

- XP3 补 `FileName`、每流 context、六参数 extraction、三参数 content 和 `[action, context]`；action=1 整文件预取接入读路径。xor 快路径不绕过 context，换游戏时清理旧会话。
- `blendRect`、`stretchPile`、`stretchBlend`、`affineBlend` 补实际绘图。pile 使用 `bmAlphaOnAlpha/bmAlpha`，blend 使用 `bmCopyOnAlpha/bmCopy`；矩阵/三点重载、默认参数与废弃 holdalpha 按参考核对。
- 当轮主机 CTest 5/5；`krkrsdl2_debug_1789987281.log` 输出 `[port-test] COMPLETE checks=12 local updates active`；XP3 日志 `krkrsdl2_debug_1789993737.log` 输出 `PAYLOAD PASS` / `STARTUP PASS`。
- 五个标题连续启动/退出验收字体与重启生命周期，用户确认当轮普通游戏正常；不扩展到所有 Layer/XP3 API。

| 历史产物 | 字节数 | SHA-256 |
|---|---:|---|
| 9 月 21 日正式 NRO | 28,331,285 | `4FB98B023EC49CE2FC0FB917BEEA53495FDEAD43606543D6572BBE2EBD12DF94` |
| Layer 夹具 NRO | 28,244,509 | `44A54E5C5D472117CEFB4D63ECFCF548A0EA75D717A3D4D7EE5E6FBE98953E0D` |
| `contract.xp3` | 17,700 | `FCDBCE37BF3376BB3971DE0025980CFE8C2E08171F0FF6E44670D38B3F17E7A5` |
| `xp3filter.tjs` | 1,407 | `EB8617F68DA5AA0B5CF69ABF9C9D6BF8A4C3786DEE34F04F9C3FB70DE656434E` |

### 影片、菜单与清理：2026-09-27 至 10-01

影片定位通过 demux seek、codec flush、PCM/重采样与旧帧清理、关键帧预解码、时间/事件重置完成，保留 playing/paused/stopped 状态。overlay/layer 更新共用分段循环边界，暂停定位不循环。两路径专项各完成 9 项运行检查；帧号仍按固定 FPS 换算，不宣称 VFR 精确定位。

菜单补 Add/Insert/Remove、跨父移动、祖先环保护、index、单选组和失效清理。native 类只经显式 `menu.dll` 登记，游戏菜单不强制进入 native 树。popup 支持子菜单、分隔/勾选/隐藏/禁用；快捷键按当前窗口与修饰键匹配，菜单拦截背景游戏、配对 up 和旧延迟点击。

脚本树 36 项、native 树 20 项及键盘/手柄/触摸已运行。公共 MenuItem 曾被撤掉，改变 LimeLight 能力分支；恢复原可见性后用户确认标题、设置、剧情正常。新增能力分支 fixture 已收录，最终版本未单独重跑，旧检查数量不代表新分支验收。`addHook/removeHook` 是历史兼容 no-op，公开 K2 MenuItem 没有这两个成员，不称为完整 hook。

10 月 1 日在 C++ 构造中初始化 `Window.Owner`，修复脚本派生类不调用 Window 构造时，新菜单清理引用未初始化 Owner 的崩溃。正常菜单关闭继续执行，用户确认同一快速读档正常。实际源码提取测试覆盖未 Construct、构造前/后异常、正常菜单与底层创建失败，并提供旧/新流程因果对照，见 [窗口生命周期](RUNTIME_NOTES.md#quickload-owner)。

### 独立语音内容探测：2026-10-01

`src/core/sound/sdl2/FFWaveDecoder.cpp/.h` 复用存储流与静态 FFmpeg，登记在专用解码器之后，失败返回空。采样率、声道、精度和 sample-granule 契约按消费者核对；planar 转交错，seek 重建 codec 并保留 AAC priming，EOF 不重播。MOV/AAC 静态符号与登记已确认，无需另建依赖。

三份实际 AAC-in-.ogg 和两份 mono/stereo 合成样本完整 PCM 与同版 CLI 逐字节一致；起点重播、尾部短读、重复 EOF 和无效输入已测试。随机中途 AAC seek 有预测/噪声状态数值差异，定位为零样本偏移，不称为全部 bitexact。实际游戏新日志命中 FFmpeg，BGM/SE 保持 Opus，用户确认人物语音正常；见 [语音记录](RUNTIME_NOTES.md#aac-voice)。

## 当前实施顺序

先处理实际命中的故障与公开 API，再补长尾；已完成项不再列为待做。

| 顺序 | 尚待处理 | 验证要求与来源 |
|---|---|---|
| 1 | 持续低帧：E-mote 像素工作、CPU Layer 合成、冷资源尖峰；完整 GPU Layer 管线 | 对齐同游戏操作、包身份和 profiler 分母；像素对照后实际验收。保留原画质、CPU 回退与游戏时钟 |
| 2 | 影片倍率/停止帧/声像/多轨选择和禁用、PhaseVocoderDSP | K2 KRMoviePlayer、WaveSoundBuffer/DSP、KRKRZ 停止事件；覆盖 seek/暂停/重播/音画时钟。stopFrame 非当前已暴露 TJS 属性 |
| 3 | ZIP/TAR 内容识别 | K2 ZIPArchive/TARArchive；接实际 creator 链，覆盖入口脚本、成员流、优先级和错误输入 |
| 4 | extrans、LayerExDraw/GDI+、layerExMovie、perspectiveCopy、AreaAverage/shrinkCopy、命中的 BPG/PVRv3 | 公开 K2/krkrsdl3 参考；真实素材/像素核对，crossfade 回退和类名探测不算实现 |
| 5 | TextRender 选项/等待与历史增量；E-mote 颜色/timeline/同步/物理 | 私有契约使用原宿主/运行探针；setColor、blend/fade、skip/pass、flags、assign、风/outline 逐项贯通 |
| 6 | System.inputString、win32dialog 模板 UI、Pad、enableTouch/触摸轮询 | 按窗口维护 ID/坐标/生命周期与交互结果；平台不支持的操作明确表达失败 |
| 7 | fstat/scriptsEx/json/sqlite3/windowEx/lzfs/PackinOne 剩余成员 | 有调用证据再移植；保留 Storages.moveFile、Layer.stitchWrappedCopy。`Scripts.saveDataPack`、`WaveSoundBuffer.StkFreeVerb` 日志异常仍是边界 |

TextRender `setOption/keyWait`、脚本 plugin wrapper 的 load/unload/isLoaded，以及 System.shellExecute/system 也需核对真实动作与失败返回。它们不由网格加速解决，不贸然覆盖游戏自己的包装链。

## 历史审计与方法纠正

| 历史记录 | 当时证据 | 当前解读 |
|---|---|---|
| 2026-09-15 krkrsdl3 backlog | `c014d30`；列 FF 音频、ZIP/TAR、extrans、Draw、AlphaMovie、Raster 和辅助插件 | FF 音频、AlphaMovie、copyRaster、moveFile 已有实现，余项并入当前顺序。平台选择不算 API 缺口 |
| 2026-09-19 K2 对照 | 同名函数扫描发现 FreeTypeFontRasterizer 缺 `ApplyFallbackFace`，构建 `886e35b8` 修复 | 同名仍须比较函数体；旧 Window 73/79 成员、插件行数和“领先”描述不证明功能完整 |
| 2026-09-27 空占位审计 | 起点 `dc284da371ce50c71db62b9654bf8127b5c52ed9`；注册 → 平台条件 → 后端 → 状态/回调，同时核 CMake/source list | 空 position/frame、popup、共享根和缺 FFWave 的结论已被后续取代，其它接口逐调用判断 |

9 月 19 日扫描线索包括 TJS String IndexOf/SubString/Trim、内部 GetLength、文本编码转换、位图 AssignTexture/InternalBlendText/IsOpaque、LayerManager 纹理/缓冲、字符分配与 locale 无关分类。这些是历史候选，不能直接视为当前确认缺失。Vorbis Render/SetPosition、float PCM 转换、连续事件线程也要检查实际替代实现。

历史实际故障提供更强证据：日文字体缺中文字形需要 fallback；TextRender 履历只有名字暴露 push/flush 与增量时序；公共 MenuItem 可见性改变暴露游戏能力分支；快速读档暴露 native attach 早于脚本构造的寿命边界。后两项原因与用户复测见 [运行记录](RUNTIME_NOTES.md)。

- `krkrsdl2_link_*` 是合法静态链接锚点；父类默认事件、无 HWND 的平台回调可能合法无操作。
- `fftgraph.drawFFTGraph` 的空实现与公有 K2 一致，仍有局限，但不是本地新增回归。
- 不在 source list 的 WaveImpl、FAudio 宏排除的 OpenAL 不能证明默认音频无效。NullAudioDevice 只在显式 null 设备路径可达，其完成通知不完整属于条件缺口。
- Switch 不执行 Win32 视频的 ZoomRectangle 调用，不把禁用路径列为当前画面主因。
- 公有源码缺失不证明 Release/APK 缺失；同名 DLL 被忽略不证明实现。BPG/PVR 等按所比较工程和版本判断，旧 krkrsdl3 对照不代替整个 K2 的结论。

## 完成定义

1. 源码：签名、默认参数、错误、状态、回调、引用和会话清理对齐固定来源，平台差异有边界。
2. 构建：实现进入 CMake/source list 和完整 Switch NRO，正式 RomFS 保留正常启动器与兼容文件。
3. 主机/专项：覆盖实际消费者与失败控制；绘制比较完整输出/脏区，音频比较 PCM/定位/结束，寿命比较正常/异常构造。
4. 运行：核 GUI 路径、NRO 哈希/构建标识，再执行实际报错动作；标题到达或 fixture 通过不代替用户操作。
5. 发布边界：模拟器与物理 Switch 分别记录；未做真机专项、标准上游全套或所有游戏回归时不扩大结论。
