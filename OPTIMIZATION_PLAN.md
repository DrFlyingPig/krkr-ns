# KRKR-ns 性能优化方案 (OPTIMIZATION_PLAN)

> 2026-09-12 稳定性更新：真机位图分配失败已在模拟器复现，加入大位图独立内存区并修复字体库提前释放；选项居中已由用户截图确认。最终回归通过 22,813 项校验，真机长期稳定性待复验。详见 [FREEZE_DIAGNOSIS.md](FREEZE_DIAGNOSIS.md)（P63）。

> 2026-09-12 更新：菜单首次使用、PSB、字体、固实归档与存档公共路径优化已实现，完整验证与真机复验方法见 [MENU_PERFORMANCE.md](MENU_PERFORMANCE.md)（P62）。下文保留早期阶段的瓶颈和规划，不代表当前代码仍缺少所有列出的优化。

> 2026-09-07 起。目标：充分利用 Switch 4 核 CPU + Tegra GPU，把 KRKR-ns (krkrsdl2) 从"特别卡"优化到 **对白/普通场景 ≥60fps、E-mote 动画场景 ≥30fps**。全程真机为准（模拟器 mesa 软件 GLES 有视口钳位 bug，只做逻辑链路验证）。

## 0. 已确认的瓶颈（代码级证据，2026-09-07 核实）

| # | 瓶颈 | 证据 | 影响 |
|---|---|---|---|
| 1 | **simde→NEON SIMD 内核是死代码** | `src/core/environ/sdl2/DetectCPU.cpp:43` TVPCPUType 恒=0（x86 CPUID 被宏排除）；`TVPGL_SSE2_Init()`（`external/krkrz/visual/gl/blend_function_sse2.cpp:1292`）被 `if(TVPCPUType & TVP_CPU_HAS_SSE2)` 门控 → AlphaBlend/ConstAlphaBlend/CopyOpaqueImage/Ps* 等全走标量 `_c`。sse2 .obj 确实编进构建（`build-switch/.../blend_function_sse2.cpp.obj`） | 合成内核无 SIMD，纯标量 |
| 2 | **每帧全表面上传** | `src/core/sdl2/SDLApplication.cpp` `__SWITCH__` 覆盖脏矩形（全幅 UpdateTexture ≈3.7MB/f，实测 upMB/f=3.5）+ RenderClear+全幅 RenderCopy | 对白场景大部分浪费 |
| 3 | **E-mote 默认 CPU 后端** | `src/plugins/emoteplayer/TVPCompositor.cpp:24`（`emote-gl.txt` 才开 GL）；GL 模式也要 glReadPixels→字节交换→回拷图层位图→CPU 再合成 | 实测 9.3ms/笔 × 12-20 笔 ≈ 4-6 FPS（全分辨率） |
| 4 | **GPU 只做最终 blit** | 主合成管线零 FBO/着色器（KRKRZ_ENABLE_CANVAS 默认关）；`tTVPBasicDrawDevice` 全软件（tvpgl.c） | 4 核 A57 承担全部像素活 |
| 5 | libpng NEON 关 | `CMakeLists.txt:186 PNG_ARM_NEON_OPT=0` | 图片解码慢 |
| 6 | XP3 解压单线程 | `external/krkrz/base/XP3Archive.cpp:644-695`，1MB 段缓存 | 读盘/转场卡顿 |
| 7 | 视频 stub | `src/core/visual/sdl2/VideoOvlImpl.cpp` no-op | 无 OP/ED |
| 8 | 多核现状 | krkrz DrawThreadPool 已开（TVPDrawThreadNum=0 auto，`SysInitImpl.cpp`），大块混合按扫描线拆分；音频解码/异步图片加载各有线程；TJS VM 单线程（不动） | 部分已并行 |

## 1. 阶段划分（go/no-go 逐级）

- **Phase 0 ✅（已完成 2026-09-07）**：真机剖析基线。埋点：
  - `src/core/sdl2/KrkrNSProf.h` + `SDLApplication.cpp` 实现：主循环四段（ev/disp/tick/wait，`Application.cpp Run()`）、引擎软件合成（`LayerManager.cpp UpdateToDrawDevice` 括起 CompleteForWindow）、合成→surface memcpy（`SDLBitmapCompletion.cpp`）、上传/呈现（TickBeat）；每 60 更新帧一行 `[prof]`，含 upMB/f。
  - 启动环境一行 `[ns] env: cpus= vsync= refresh= pos=`。
  - 复用 `trace-render.once`、`[emote] * profile`、心跳/stage。
- **Phase 1 ✅（代码完成 2026-09-07，真机待测）** CPU 快赢：①③✅ simde SIMD 激活（绕过 x86 门控，排除 AVX2；`tvpgl-scalar.txt` 掩位回退）——模拟器同场景 E-mote 每笔 9.37→2.03ms、E-mote 帧 compose 182.9→18.7ms；②✅ 脏矩形上传默认关、`dirtyrect-update.txt` 标记开启（**格式链已核对：引擎 B,G,R,A 与 surface 掩码天然一致，无需改 RGBA8888**）；④✅ XP3 段缓存 1MB→8/16MB；③libpng NEON **暂缓**（vendored libpng 1.6.5 无 arm/ 源，需换版本）；⑤多核：tvpgl 大块混合本就按扫描线拆分（池=SDL_GetCPUCount），阈值暂不动，待真机基线再定。
- **Phase 2 E-mote 上 GPU**：默认 GL 后端（真机硬件 GLES；模拟器标记文件兜底 CPU）；砍回读跳（渲染目标直作图层纹理，免 glReadPixels+字节交换+CPU 再合成）；补缺失 API（setSize/setLayerMatrix…，参照 krkrsdl3 DrawDeviceD3D）。
- **Phase 3（go/no-go，Phase 1+2 未达标才启动）全套 GPU 合成**：Kirikiroid2 架构 — 每图层 GL 纹理 + 着色器目录（`RenderManager_ogl.cpp:2572-3356` 的 ~50 种混合；dst-alpha 族用临时纹理双采样回退，Tegra X1 无 framebuffer-fetch）+ `glTexSubImage2D`+`GL_UNPACK_ROW_LENGTH`(ES3) 部分更新 + 主图层纹理全屏 quad 呈现 + 文字字形缓存 GPU 快速路径。软件路径保留兜底。
- **Phase 4（后续独立阶段）视频播放**：移植 WA2-ns 已验证管线（`WA2-ns/src/wa2/video.cpp`）：最小静态 FFmpeg（仅 LGPL、NEON、`-mcpu=cortex-a57`）、专用解码线程（4MiB 栈）、无锁 SPSC 帧队列、解码侧节奏（pts=frame_index/fps）、自定义 AVIO 支持 sdmc:/、解码线程上 swscale SWS_POINT、音频大环缓冲、1Hz 心跳。替换 VideoOvlImpl stub。

## 2. 真机验证协议（每阶段）

1. `bash build_nro.sh` → 产物 `build-switch/krkrsdl2.nro`（用户自己复制到真机，我绝不写用户 Switch）。
2. 用户真机跑规定场景（菜单/对白/E-mote 各约 1 分钟），MTP 只读拉 `sdmc:/krkrsdl2_debug.log` + trace BMP。
3. 对照 `[prof]` 各段 ms 与 `upMB/f`，逐阶段给前后对比；达标即收，未达标进下一阶段。

## 3. 模拟器基线（2026-09-07，软件 GL/单核，仅结构参考）

```
[prof] 对白帧 fps=60.5 | compose=10.7 surfcopy=0.6 upload=1.6 present=7.1 | upMB/f=3.5
[prof] E-mote帧 fps=5.0 |  disp=183.01 compose=182.91 | [emote] SW avg=9.37ms/call calls=1440
```
归因：**dispatch ≈ compose**（既有帧的 CPU 时间几乎全在软件合成）；E-mote 场景 9.4ms/笔 × ~20 笔 = 整帧。真机数字待用户基线测试。

## 4. 目标与达标线

- 对白/普通场景 ≥60fps；E-mote 动画场景 ≥30fps（WA2-ns 在 Switch CPU 合成也以 30 为线）。
- CPU 合成瓶颈期可接受 30fps 节奏；GPU 合成就绪后按 VSYNC 全速。

## 5. 约束（沿用既定约定）

- 不改用户 Switch 任何东西；NRO 由用户复制；日志 MTP 只读。
- 产物固定 `build-switch/krkrsdl2.nro`；补丁记录进 PATCHES.md；TJS VM 留主线程。
- 模拟器仅逻辑链路；GPU 与性能数字以真机为准。
