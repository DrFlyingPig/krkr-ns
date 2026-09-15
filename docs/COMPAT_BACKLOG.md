# KRKR-ns 兼容性缺口清单（对照 krkrsdl3）

> 盘点：2026-09-15。对照对象：**krkrsdl3**（SDL3 重写版），本地参考 `.zcode/upstream-krkrsdl3`（官方 main `c014d30`）。
> 只列「对方有、我们没有」的能力，按对未修改商业标题的影响 × 实现成本排序。
> 已核实持平的部分不在此列（文本编码探测、图像格式、FFmpeg 影片、基础插件族等，见文末「非差距」）。

## 优先级总表

| # | 优先级 | 缺口 | 影响面 | 参考实现（krkrsdl3） | 规模 |
|---|--------|------|--------|----------------------|------|
| 1 | **P0** | FFmpeg 音频解码（mp3 / aac / wma / flac …） | 用 mp3/aac 语音或 BGM 的标题 → 全程静音 | `plugins/wuffmpeg.cpp` | 428 行 |
| 2 | **P0** | ZIP / TAR 压缩包 | 以 zip/tar 分发的标题 → 打不开数据包 | `plugins/Kirikiroid2/ZIPArchive.cpp`、`TARArchive.cpp` | 142 + 263 行 |
| 3 | **P1** | extrans 转场（wave / mosaic / turn / rotatezoom / rotatevanish / rotateswap / ripple） | 用 extrans.dll 的标题 → 转场退化为 crossfade（魔女的夜宴等已命中） | `plugins/extrans/` | 约 9k 行（含 4.5k 表） |
| 4 | **P1** | layerExDraw 的 GDI+ 模拟 | 靠 layerExDraw.dll 自绘 UI 的标题 → 界面画错/缺失 | `plugins/LayerExDraw/LayerExDraw.cpp` | 2851 行 |
| 5 | **P1** | XP3 content filter + 6 字段 filter info（含 FileName） | 用 content filter 的加密/保护方案 → 解密失败 | `core/archive/XP3Archive.cpp:415`、`plugins/xp3filter.cpp` | 611 行 |
| 6 | **P2** | perspectiveCopy（layerExPerspective.dll） | 少量特效调用；已确认自有标题无调用 | `plugins/LayerExPerspective.cpp` | 143 行 |
| 7 | **P2** | LayerExMovie：`Layer.openMovie/startMovie/stopMovie` | 影片层；自有标题暂无调用（overlay/mixer 路径已通） | `plugins/LayerExMovie.cpp` | 353 行 |
| 8 | **P2** | AlphaMovie（.amv 带 alpha 影片） | 用 .amv 的标题 → 影片不显示 | `plugins/AlphaMovie.cpp` | 2089 行 |
| 9 | **P2** | LayerExRaster / LayerExAreaAverage / shrinkCopy | 图像裁切、均值缩放类调用 | `plugins/LayerExRaster.cpp`、`LayerExAreaAverage.cpp`、`shrinkCopy.cpp` | 117 + 230 + 579 行 |
| 10 | **P2** | json / fstat / scriptsEx 剩余 / sqlite3 / windowEx / lzfs / PackinOne | KAGEX 系脚本与个别标题的辅助 API | `plugins/{json,fstat,scriptsEx,sqlite3,windowEx,lzfs,PackinOne}.cpp` | 1k–3k 行/个 |
| 11 | **P3** | 小成员补齐（见下） | 个别脚本调用 | `core/script/tjsNativeStorages.cpp:158` 等 | 每项几行–几十行 |

---

## P0 — 通用缺口（建议先做）

### P0-1 FFmpeg 音频解码（mp3 / aac / wma / flac）
- **现状**：`krkrsdl2/external/krkrz/sound/WaveIntf.cpp` 仅登记 RIFF(WAV) / Vorbis / Opus / TCWF；mp3/aac 标题静音（不影响启动，但影响大量标题的可玩性）。
- **目标**：接入 FFmpeg 波形解码器——不筛扩展名，交 libavformat 探测（对方 `wuffmpeg.dll` 即此策略）。
- **接入点**：`WaveIntf.cpp` 的解码器注册链。静态 FFmpeg 已随影片路径集成（`krkrsdl2/src/core/visual/sdl2/SwitchMovieOverlay.cpp`，构建产物 `out/ffmpeg_switch`），可直接复用，无需新增依赖。
- **验证**：含 mp3 语音/BGM 的标题不再静音；建议新增 `[wave]` 解码日志（编解码器名 + 采样率）。

### P0-2 ZIP / TAR 压缩包
- **现状**：只认 XP3 + 7z（按内容嗅探）。
- **参考**：`.zcode/upstream-krkrsdl3/plugins/Kirikiroid2/ZIPArchive.cpp`（142 行）、`TARArchive.cpp`（263 行）；注册点同目录 `kirikiroid2.cpp:78`。
- **接入点**：`krkrsdl2/src/core/base/sdl2/StorageImpl.cpp` 的 `TVPOpenArchive` 链条，挂 `TVPRegisterArchiveFormat`。
- **验证**：zip/tar 打包的最小工程可直接启动；`[miss]` 不再报入口脚本缺失。

## P1 — 表现正确性（自有标题已命中或易命中）

### P1-3 extrans 转场
- **现状**：缺失转场回落到 crossfade（日志 `[trans]` 可见），魔女的夜宴的 rotatezoom 等显示为淡入淡出。
- **参考**：`.zcode/upstream-krkrsdl3/plugins/extrans/`——wave / mosaic / turn / rotatezoom / rotatevanish / rotateswap / ripple，均经 `TVPAddTransHandlerProvider` 登记。
- **接入点**：引擎已有 crossfade 回落机制，替换为真实 handler 即可；注意转场表的授权/尺寸约定按我们现有约定移植。
- **验证**：`[trans]` 不再出现回落日志；画面为真实转场效果。

### P1-4 layerExDraw（GDI+ 模拟）
- **现状**：仅保证 `typeof GdiPlus` 探测不炸（k2compat），没有绘图后端；用 layerExDraw.dll 自绘 UI 的标题界面会画错或缺失。
- **参考**：`.zcode/upstream-krkrsdl3/plugins/LayerExDraw/LayerExDraw.cpp`（2851 行，`GdiPlus` + 大量 Layer 绘图 API）。
- **验证**：命中该 dll 的标题进入自绘界面（设置页/画廊等）逐项对比截图。

### P1-5 XP3 content filter + filter info 6 字段
- **现状**：仅 4 参 extraction filter（`(FileHash, Offset, Buffer, BufferSize)`），content filter 未接。
- **参考**：`.zcode/upstream-krkrsdl3/core/archive/XP3Archive.cpp:415`（content filter，可请求整包获取）、`plugins/xp3filter.cpp`（611 行）；info 结构 `{SizeOfSelf, Offset, Buffer, BufferSize, FileHash, FileName}`。
- **验证**：使用 content filter 的保护方案包可正常解密进入；`[xp3]` 相关日志确认回调被调用。

## P2 — 插件族长尾（遇到再移植，参考源码均已在本地）

- **P2-6 `perspectiveCopy`**：`plugins/LayerExPerspective.cpp`（143 行）。注意它依赖透视采样原语，移植时按我们 Layer 的 blit 管线实现。
- **P2-7 `Layer.openMovie/startMovie/stopMovie`**：`plugins/LayerExMovie.cpp`（353 行），接线到既有 `SwitchMovieOverlay`，勿照搬对方的影片接口。
- **P2-8 `AlphaMovie`（.amv）**：`plugins/AlphaMovie.cpp`（2089 行）。
- **P2-9 `LayerExRaster`（copyRaster，117 行）、`LayerExAreaAverage`（stretchCopyAA，230 行）、`shrinkCopy`（Layer.shrinkCopy/shrinkCopyFast，579 行）**。
- **P2-10 辅助插件族**：`json.dll`（Scripts.evalJSON/saveJSON/toJSONString）、`fstat.dll`（Storages.fstat/dirlistEx/dirtree 等）、`scriptsEx` 剩余成员、`sqlite3.dll`、`windowEx.dll`、`lzfs`（存储介质）、`PackinOne`（AffineSourceMovie）。

## P3 — 小 API 补齐（改动小，随手可做）

- `Storages.setTextEncoding`（对方为 native，`core/script/tjsNativeStorages.cpp:158`；我们只有 `-readencoding` 命令行开关）
- `System.inputString`
- `Storages.dirtree / dirlistEx / getMD5HashString / searchPath / getTemporaryName`（fstat 族）
- `Layer.stretchPile / fetchImageSize`

## 非差距（两家都没有 / 我们反而领先，别追）

- **图像**：BPG / PVR / GIF / JXR（Switch）两家都没有；WebP 两家都有。
- **Susie 归档、MIDI**：两家都没有 / 均关闭。
- **`Layer.stitchWrappedCopy`（layerStwCopy 门控）**：krkrsdl3 全库没有，我们已有原生实现（LimeLight 依赖）——保留，别被"对齐上游"带偏。
- **平台差异**：对方 SDL3 / GL·Vulkan·软件后端 / D3D 插件（Windows），我们是 SDL2 / GLES——属于目标平台差异，不算兼容缺口。
