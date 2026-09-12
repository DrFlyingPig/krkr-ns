# KRKR-ns 补丁记录 (PATCHES)

> 📋 **开发全过程复盘(需求/流程/踩坑)见 [DEVLOG.md](DEVLOG.md)**——本文件只列代码补丁的技术细节。

相对上游 krkrsdl2 (pinned `bf207f2`) 的全部本地改动。同步上游时逐个重新应用。

## 已应用补丁 (源码层)

### P63: 长剧情位图分配、字体生命周期与选项居中（2026-09-12）

- 真机日志出现 1920×1440 位图分配失败；模拟器复现通用堆约 2.38 GiB 空闲却无法分配约 17 MiB 连续位图。Switch 改用大位图专用内存区，空闲页可合并复用，保留写时复制和像素地址稳定性；完全空闲区保留预算 128 MiB，内存压缩时释放。
- 位图记录保存实际容量，新增周期内存计数及分配失败/重试快照，避免把进程保留的整个堆误判为活跃对象用量。
- 修复单个 TextRender 光栅器析构时关闭全局 FreeType 库、使其他字体 face 失效的问题；全局库统一在引擎最终清理阶段释放。
- 补齐 TextRender `valign=0` 居中分支，按当前字体 ascent 定位；用户实际游戏截图已确认居中。
- 验证：5 项本地测试、22,813 项完整 ARM 引擎校验通过，包括并发位图复用、300 轮大图写时复制、16 轮文字对象创建销毁。内存区版本约 18 分钟真实剧情运行未复现位图分配失败；最终版真机长期复验仍待完成。原始证据、复现方法与边界见 [FREEZE_DIAGNOSIS.md](FREEZE_DIAGNOSIS.md)。

### P62: 菜单首次使用、存档与资源加载优化（2026-09-12）

- FreeType 字号度量缓存改为 256 字符分页，避免新字号首次初始化约 6.25 MiB；补齐 U+FFFF，保留原字形度量和字号隔离。
- 文本/结构化存档使用 16 KiB 缓冲，UTF-16LE 直接写入，合并细碎 zlib/文件调用；完成压缩前刷新，存档模式与读回内容保持兼容。
- PSB 媒体使用共享只读资源切片，避免枚举、注册、打开时复制全量数据；预留大字典哈希容量，共享写时复制字符串，重载后旧流与旧 root 仍有效。
- 固实 7z 保留一个最多 16 MiB 的共享解压块，打开相邻成员不再重复解压；保留成员 CRC 和边界检查。
- 修正图像缓存同键替换重复计费、插入后超预算问题，缓存命中免去路径日志；增加 `[slow]` 与单次 `[stall]` 记录定位真机首次点击。
- 验证：4 项本地测试通过；完整 ARM 引擎前后各通过 22,151 项校验，文字和测试视口像素一致，六种存档内容一致。模拟器中 10,000 键 PSB 解析 102→29 ms，固实归档 32 次读取 4,539→269 ms；**真机待复验**。实现边界、完整计时和复现命令见 [MENU_PERFORMANCE.md](MENU_PERFORMANCE.md)。

### P1: romfs 构建 copy 替代 symlink
`krkrsdl2/CMakeLists.txt` — Windows 无管理员权限无法建符号链接。

### P2: Switch 链接补全 -L 搜索路径
`krkrsdl2/CMakeLists.txt` — pkg_check_modules 吞掉 -L,patch 追加 `$ENV{DEVKITPRO}` 路径。

### P3: 平台初始化阶段日志 (KrkrNSLog)
- 新增 `krkrsdl2/src/core/sdl2/KrkrNSLog.h` — 单一 fd 写入 `sdmc:/krkrsdl2_debug.log`(不与其他日志文件争用,避免模拟器 sdmc 上的堆损坏)。
- `SDLApplication.cpp` / `Application.cpp` / `ScriptMgnIntf.cpp` / `StorageIntf.cpp` / `StorageImpl.cpp` / `DebugIntf.cpp` / `SystemImpl.cpp` 大量启动与存储阶段标记。

### P4: Switch 存档/数据路径
- `environ/sdl2/ApplicationSpecialPath.h` — `GetDataPathDirectory` 在 `__SWITCH__` 下返回 `"file://romfs:/"`(游戏数据 = RomFS;存档重定向留待 Phase 4)。
- `environ/sdl2/Application.cpp` — `ExePath()` 在 `__SWITCH__` 下返回 `"file://romfs:/krkrsdl2.nro"`。
- `sdl2/SDLApplication.cpp` — 启动时 `chdir("romfs:/")` + 确保 `sdmc:/switch/krkrsdl2` 存在。

### P5: 存储层设备路径快速通道
`base/sdl2/StorageImpl.cpp` `GetLocallyAccessibleName`:
- `"file://?/<dev>"` 与 `"?/<dev>"`(管理器剥离 `file://` 后的形态)直接映射为 `<dev>`(即 `romfs:/`、`sdmc:/`)。
- 原因:上游 switch hack(`file://?/` 前缀)的目录遍历逻辑在冒号设备路径上产出错误本地名(`"."`),导致启动脚本永远找不到。
- 文件列举:`d_type == DT_UNKNOWN` 视为文件(stat 兜底在模拟器 fsdev 上会挂起,故移除)。

### P6: 渲染器/窗口/纹理全链路 (Switch 显示修复,2026-09-03 系列)
`sdl2/SDLApplication.cpp`,最终组合拳(**模拟器+真机双端全屏红色验证**):
1. 窗口尺寸:手持 1280×720 / 底座 1920×1080(`appletGetOperationMode()` 判断),并**保留** `SDL_WINDOW_FULLSCREEN_DESKTOP`(onsyuri 实证组合:FULLSCREEN + 显式尺寸)。
2. 构造器:在 `this->texture = nullptr;` 重置**之后**创建纹理+表面(原址创建会被下一秒清零——这是"黑屏→少量红色"的关键修复),附上 `SDL_RenderSetLogicalSize(1280,720)` 全屏缩放;TickBeat 增加纹理空置兜底创建。
3. 根因链:SetPaintBoxSize 在本平台从不被调用 → 纹理从未创建 → RenderCopy 无内容 → 纯黑。补丁在构造器补齐 texture+surface 整条管线(surface→UpdateTexture→RenderCopy→RenderPresent)。
4. **渲染器仍用 ACCELERATED|VSYNC + mesa GLES2**(软件渲染此前已证实不可行:switch 驱动无窗口帧缓冲)。
5. 测试脚本层:`new Window + setInnerSize + Layer setSize(1280,720) + fillRect` 全屏红(见 `data/startup.tjs`)。

**模拟器注意**:Nextendo 已能显示本引擎帧(窗口标题变为游戏、全屏红)——之前的"正在加载"就是客机无帧可送。nvmap/vi 直写帧缓冲路径(0xf59/0x1272)在模拟器+真机均不可用,已弃用,保留代码仅为参考。

### P8: 游戏加载模式 (sdmc game dir, Phase 6b)
- `SDLApplication.cpp` `switch_game_mount()`:启动时枚举 `sdmc:/switch/krkrsdl2/game/`;发现内容(散文件或 `*.xp3`)→ **游戏模式**:
  - 每个 `*.xp3` 通过 `TVPAddAutoPath("sdmc:/.../xxx.xp3>")` 挂载(archive 内文件名透明可用,未加密 xp3)
  - `TVPDataPath` 切到游戏目录 + `chdir` + **`TVPStartupScriptName = "sdmc:/.../game/startup.tjs"`(绝对路径!** cwd 相对名在模拟器 fs 上不可靠——模拟器忽略 chdir,曾导致误加载 romfs 的 startup.tjs)
  - 引擎随即加载**游戏自己的 startup.tjs**(KAG 标准布局)而非内置 romfs 测试脚本
- 无游戏内容 → 保持 romfs 测试模式(内置多层测试脚本)。
- **已验证(模拟器)**:`Loading startup script : sdmc:/.../game/startup.tjs` + 游戏脚本正常执行、画面绘制、无异常。
- 真实 KAG 游戏兼容性(字体/System 组件/动画)→ 待真实 xp3 实测。

### P7: 手柄→鼠标/键盘事件合成 (Switch 输入,Phase 3)
`sdl2/SDLApplication.cpp`,`#ifdef __SWITCH__` 全块。**模拟器已验证:手柄按钮 → 合成 SDL 鼠标/键盘事件 → TJS onMouseDown/onKeyDown 全链路到达**(`EVT down btn=1`=右击、`EVT key 32`=Space、坐标自动换算 960,540→640,360)。
- `switch_process_gamepad_input()`(TVPWindowWindow 成员):每帧轮询全部 `SDL_GameController`(轮询不依赖事件流;`refresh_controllers` 在窗口创建时 + 设备增删时重开),边沿检测按钮,通过 `SDL_PushEvent` 注入合成事件(SDL_Send* 为内部 API,不可用;**注意 PushEvent 不更新 SDL 内部鼠标/键盘状态,shift 状态位缺失,可接受**)。
- 映射:A=左键、B=右键、X=Return、Y=Space、Minus=F5、Plus=Escape、L3=LCTRL(KAG skip)、R3=F7、L=滚轮上、R=滚轮下、方向键=光标微移(150ms 重复)、左摇杆=光标移动(死区 8000,全偏 14px/帧)。
- 事件坐标用**窗口像素**(与触摸事件同空间,引擎 TranslateWindowToDrawArea 自动换算;实测 docked 1920×1080 → 1280×720 换算正确)。
- 保留引擎原生 VK_PAD* 事件流(脚本可二选一)。
- **关键约束**:krkrsdl2 的 BasicDrawDevice **只支持 1 个顶层 LayerManager**(`AddLayerManager` 显式拒绝多于 1 个;第二个 `new Layer(w,null)` 会 Internal Error)——**测试/演示脚本必须单顶层图层**(KAG 多图层待 Phase 6 评估:需要放开限制并合绘)。
- 测试脚本 `data/startup.tjs`:单图层,鼠标轨迹白方块、点击画色块、按键整屏着色 + `Debug.message` 事件日志(在 __SWITCH__ 下镜像到 `krkrsdl2_debug.log`,即 TJS 侧也能打日志!)。
- **按钮枚举坑**:`tTVPMouseButton` = `mbLeft=0, mbRight=1, mbMiddle=2`(tvpinputdefs.h)——脚本判断右键必须用 `==1` 而非 `==2`(真机 A/B 都画绿即此因;模拟器日志 `EVT down btn=1` 佐证合成本身正确)。
- **键位语义坑(NS vs Xbox)**:`SDL_CONTROLLER_BUTTON_*` 遵循 **Xbox 位置语义**(A=下、B=右、X=左、Y=上)。NS 物理键位与之镜像(物理 A=右下、B=下、X=右上、Y=左上)。**合成映射按物理键交换**:SDL-A 位(→NS 物理 B,下)= 左键确认;SDL-B 位(→NS 物理 A,右下)= 右键取消;SDL-X 位(→NS 物理 Y,左上)= Space;SDL-Y 位(→NS 物理 X,右上)= Enter。**真机已验证(2026-09-03):NS 物理 B 按=绿(左键),NS 物理 A 按=蓝(右键)** ✅ Phase 3 收官。

## 环境层改动 (D:\devkitPro 等)

### E0: ~~EGL stub~~ → 已替换为真实 switch-mesa ✅ (2026-09-03 晚)
- **switch-mesa 20.1.0-5 已安装**(通过用户 FlClash 代理 + 浏览器下载 `dkp-libs.db` 定位文件名,官网直连 403 是 Cloudflare 脚本验证;curl 无法通过,浏览器可)。
- 安装内容:`libEGL.a`(22MB 真库)、`libGLESv2.a`、`libGLESv1_CM.a`、`libglapi.a` + EGL/GL/GLES{1,2,3}/KHR 全套头文件 + egl.pc/glesv2.pc 等。
- **M1 达成**:窗口创建 + GLES2 渲染器 + 主循环运行(日志 heartbeat 1000)。渲染器恢复原始 ACCELERATED|VSYNC(P6 已撤销)。
- `D:/KRKR-ns-tools/libEGL.stub.bak` = 旧 stub 备份,可删。
- 注意:早期在 libSDL2.a 里对 SDL_EGL_CreateSurface 打的补丁已通过恢复 `libSDL2.a.bak` 撤销。

### E0b: 网络要点
- pkg.devkitpro.org 直连 403(Cloudflare);FlClash 代理(127.0.0.1:7890)下 curl 仍被 CF 拦,但**浏览器可下载**。
- 仓库布局:pacman Server = `https://pkg.devkitpro.org/packages`,repo 名 `dkp-libs`/`dkp-toolchain`,包文件在 `/packages/<文件>` 平铺,扩展名 `.pkg.tar.zst`(不是 .xz!)。

### E1: 宿主工具
全部在 `D:/KRKR-ns-tools`:CMake 3.31.6 / ninja / pkg-config-lite / Nextendo 模拟器(Ryujinx fork,用于 NRO 调试)。

### E2: 构建脚本
`build_nro.sh`:configure → ninja 构建 → GCS 展开器补丁(见下)→ romfs 展平(数据放根)→ elf2nro → 复制到模拟器游戏列表。
- **产物命名恒为 `build-switch/krkrsdl2.nro`,不附加任何后缀**(曾用 -RED/-INPUT/-patched 等,导致模拟器/真机多副本混装;现最终 NRO 一律覆盖写 `krkrsdl2.nro`,模拟器单一入口 `portable/games/krkrsdl2.nro`。GCS patch 的中间 ELF 才叫 `krkrsdl2-patched.elf`)。
- 模拟器加载入口 = `portable/games/`(**递归扫描!** 备份目录不能放 games 下,否则列表混入旧 NRO;备份在 `D:\KRKR-ns-tools\nro_backup\`)。

### E3: GCS 展开器补丁 (模拟器兼容)
Nextendo 的 chkfeat 无条件返回 0(谎称 GCS 存在)且未实现 `gcspr_el0`/`gcspopm`,任何 C++ 异常展开都会让模拟器崩溃。`build_nro.sh` 在链接后把 5 处 `cbnz`(chkfeat 后面)改为无条件 `b`。真机无影响。


### P9: 性能剖析埋点 (Phase 0, 2026-09-07)
- 新增 `krkrsdl2/src/core/sdl2/KrkrNSProf.h` + `SDLApplication.cpp` 实现：主循环四段（events/dispatch/tickbeat/wait，`environ/sdl2/Application.cpp Run()` 段计时）、引擎软件合成（`external/krkrz/visual/LayerManager.cpp` `UpdateToDrawDevice` 括起 `CompleteForWindow`）、合成→SDL surface memcpy（`SDLBitmapCompletion.cpp`）、纹理上传/呈现（`SDLApplication.cpp` TickBeat）；每 60 更新帧一行 `[prof] updates= loops= total= fps= | seg ev= disp= tick= wait= | compose= surfcopy= upload= present= | upMB/f=`。
- 启动一行 `[ns] env: cpus= vsync= refresh= pos=`（CPU 数/垂直同步/刷新率/窗尺寸，日志自描述）。
- 头文件模式同 KrkrNSLog.h：非 Switch 为 static inline no-op，引擎 TU 可无条件调用；计时集中在 SDL 层（begin/end 括起），引擎只留成对调用。
- `<SDL.h>` include 补进 SDLBitmapCompletion.cpp。

### P10: 修复历史遗留编译错误 (2026-09-07, 非本会话引入)
构建发现两处未编译过的旧诊断代码：
- `external/krkrz/tjs2/tjsInterCodeExec.cpp` `[vmcalld]` 用了 KRKRNS_LOG 但缺 include → 补 `#ifdef __SWITCH__ #include "KrkrNSLog.h"`。
- `external/krkrz/base/StorageIntf.cpp` `[launcher]` 诊断里 `s->Release()`（tTJSBinaryStream 无 Release）→ 改 `delete s;`。
两者均为既有日志基建（见旧记忆「已就位，别删」），只修编译、不删功能。

## 性能优化方案
见 [OPTIMIZATION_PLAN.md](OPTIMIZATION_PLAN.md)：Phase 0（剖析）已完成，Phase 1（simde SIMD 激活/脏矩形上传/RGBA8888/libpng NEON/XP3 缓存）进行中，Phase 2（E-mote GPU）后续，Phase 3（Kirikiroid2 式全套 GPU 合成）go/no-go，Phase 4（WA2-ns 式视频）独立阶段。


### P11: 激活 simde SIMD 混合内核 (Phase 1, 2026-09-07)
- `src/core/base/sdl2/SysInitImpl.cpp`（TVPGL_SSE2_Init 调用处）：Switch 上 `TVPCPUType` 恒 0（x86 CPUID 被宏排除）→ simde 版 SSE2 内核全部没接线，每帧混合走标量 `_c`。现在按 `TVP_CPU_HAS_SSE|SSE2|SSE3|SSSE3|SSE41|SSE42` 置位（**不含 AVX/AVX2**——simde 模拟 256 位在 NEON 上更慢）；`tvpgl-scalar.txt` 标记显式掩位回退标量。`tvpgl_ia32_intf.h` 的 include 从 `#if 0` 块提出来。
- 模拟器结果：E-mote 每笔 9.37ms → 2.03ms（同 720p 场景）；真机数字待测。

### P12: 脏矩形上传开关 (Phase 1, 2026-09-07)
- `SDLApplication.cpp` TickBeat：默认保持全幅上传（基线不变）；`sdmc:/switch/krkrsdl2/dirtyrect-update.txt` 标记 → 走 update_rect 脏矩形上传（流式纹理保留内容 + RenderCopy 仍全幅重画背缓冲，仅 CPU→GPU 传输缩小），每 120 帧一次全幅兜底防记账残留。upMB/f 指标直接可见省了多少。

### P13: XP3 段缓存扩容 (Phase 1, 2026-09-07)
- `external/krkrz/base/XP3Archive.cpp`：TVP_SEGCACHE_ONE_LIMIT 1MB→8MB、TOTAL_LIMIT 1MB→16MB（Switch 3GB applet 内存足以容纳当前场景热段）。

### P14: libpng NEON 暂缓 (Phase 1 结论)
- 仓库内 vendored libpng 1.6.5 **无 arm/ NEON 实现源**（pnrutil 无 _neon 函数），`PNG_ARM_NEON_OPT=1` 会链接失败。需换新 libpng 才可行 → 本轮跳过（图片解码本就在异步加载线程，非帧瓶颈）。


### P15: 绘制线程数修复 — 真机 cpus=1 根因 (Phase 1b, 2026-09-07)
- 真机日志铁证：`[ns] env: cpus=1` → `TVPGetProcessorNum()=SDL_GetCPUCount()=1`（libnx sysconf 桩）→ 自适应绘制池与 E-mote 行带并行全部退化为单线程（对白帧 compose 13-16ms、E-mote 帧 114-514ms 全串行）。
- `external/krkrz/utils/ThreadIntf.cpp` TVPGetProcessorNum：`__SWITCH__` 下 `SDL_GetCPUCount()` <2 时返回 4（Switch=4×A57，homebrew 全核可用，WA2-ns 同机并行 swizzle 为证）。
- env 行补 `drawT=%d`（TVPGetThreadNum），日志自证线程数。


### P16: 池/行带诊断埋点 (Phase 1b, 2026-09-07)
- `ThreadIntf.cpp`：PoolThread 首次 spawn 打 `[pool] spawned workers=%d taskNum=%d threadNum=%d`；TVPBeginThreadTask 每 500 次打 `[pool] use: begins= avgNum= max=`。
- `EmoteSWRenderBackend.cpp` DrawMeshCpu：首个网格打 `[emote] band first: rows= bandCount= threadNum=`；每 240 次打 `[emote] band diag: calls= tall= banded= rAvg= bAvg=`。
- **模拟器冒烟铁证**：workers=3 正常 spawn、池被大量调用（begins 1500+），但 E-mote 网格 rAvg=9 行、720 次调用仅 8 次 ≥96 行 → **带拆分根本没机会生效，CPU 多线程救不了 E-mote**；1.7ms/调用是固定开销（每网格设置/纹理/蒙版处理），非像素光栅。→ Phase 2（emote-gl.txt 真机验证 GL 后端）为唯一正道。


### P17: E-mote 默认 GL + 驱动自检探针 (Phase 2, 2026-09-07)
- 真机验证 GL 后端画面正常（用户确认）→ `TVPCompositor.cpp` 默认 `wantGl=true`（`__SWITCH__`）；`emote-cpu.txt` 强制 CPU、`emote-gl.txt` 强制 GL（模拟器测试用）。
- **全靶面自检探针**（`EmoteGLRenderBackend::IsAvailable`）：创建 1280×720 FBO + 1×1 红纹理，经公开 API 画全屏 quad，回读四角验证红色——模拟器软件 GLES（钳位到 320×720）被精确检测并自动回退 CPU（模拟器实测 `GL probe FAIL corners=MISMATCH renderer=NV120` → `selected CPU fallback`）。真机应 PASS。
- 探针顺带打印 `[emote] GL probe PASS/FAIL ... renderer= version=`。

### P18: 线程池防抖 + 池计数进 [prof] (Phase 2, 2026-09-07)
- `ThreadIntf.cpp`：删掉 PoolThread 的 worker 收缩循环（上游在每次 `BeginTask(1)` 时销毁全部 worker→大任务再重建，270K 次调用=线程 churn）；worker 常驻。
- 新增 `krkrsdl2_pool_begins()/bigbegins()`（extern "C"），`[prof]` 行尾追加 `poolB= bigB=`（每窗口任务批次/大任务数，量化 E-mote 场景小图层操作负载：实测对白窗口 bigB≈1，几乎无并行机会）。


### P17b: 探针 v2 — 全幅回读完整性检查 (Phase 2, 2026-09-07)
- v1 探针（全屏红 quad 四角单像素回读）在模拟器上 **PASS**，但真实立绘右 3/4 缺失：模拟器软件 GLES 只把**整幅 ReadPixels 结果截断为 320×720**（FBO 快照内容 bbox x:0-316；实测 fullRed=25.0% 恰为 320/1280）——单像素角点读取正常，v1 漏检。
- v2 增加整幅回读：全缓冲红色像素覆盖率 ≥90% 才 PASS。模拟器 `GL probe FAIL fullRed=25.0%` → 自动 CPU；真机应 100% PASS → GL。v1 还有一坑：探针 draw 用 blend=21（enableColor=true）而 uniformColor 默认黑 {0,0,0,1} → 探针把靶面涂黑；已置白修复（v1 在真机上因此误判 FAIL 回退 CPU，run3 实为 CPU 数据）。


### P19: Phase 3 Stage 3.1 — GPU 图层合成 (2026-09-07, 构建 md5 82cb67f8)
- 新模块 `src/core/sdl2/GLComposite.{h,cpp}` + `GLCompositeBridge.h`（引擎侧桥接）：钩子= `tTVPBaseBitmap::Blt/CopyRect`（全引擎合成唯一合流点，经 `LayerManager::DrawBuffer` 注册目标位图）；命中时（dest=compose、32bpp、方法∈{copy/alpha/add}、!hda）改为向私有 ES2 FBO 画纹理 quad；呈现=全屏 quad+SDL_GL_SwapWindow，绕开 surface memcpy/UpdateTexture/RenderCopy 全链。
- 位图→纹理缓存按 (指针, w,h,pitch,版本) 键；版本在 Blt/CopyRect/Fill/StretchBlt/Assign/DrawGlyph 写入口 bump（`krkrsdl2_glc_bump_version`），光栅经 `krkrsdl2_glc_get_bitmap_raster` 桥接（模块不依赖引擎头）。
- 标记 `gpu-composite.txt` 开启；**全幅回读探针**（同 E-mote 门）自动拦截坏驱动（模拟器 25% → FAIL → CPU 兜底，实测通过）。
- 已知 v1 限制：临时位图路径（透明层子层合成）、bmSub 及 PS 系混合、hda 模式仍走 CPU；文字字形走临时位图故文字合成仍 CPU（缓存键保证不陈旧）。呈现直连 GL 后脏矩形/上传/回拷指标不再适用。
- 编译历程教训：**该 TU 与 E-mote 不同的配置下曾出现 "ActiveTexture was not declared" 假象——实为 LoadGL 内裸名漏 `gl.` 前缀**；SDL_opengles2.h 不带 PFNGL typedef（来自 SDL_opengl.h）；模块最终自建类型化函数表（53 个，免 PFN 依赖）。


### P20: Stage 3.2 v1.1 — 帧纯度守卫 + 临时位图 twin 路径 (2026-09-07, 构建 md5 f558e266)
- **纯度守卫（修 v1 正确性洞）**：`gPureFrame` 每帧重置；任何对合成目标的 CPU 回退写入（方法/hda/8bpp 不命中）→ 本帧走原呈现链（`present()` 返回 false、surface 拷贝恢复）；纯 GPU 帧才用 GL 呈现。杜绝"混合帧内容丢失"。
- **临时位图上 GPU**：非合成目标位图首次被拦截写入时创建帧内 FBO twin（FindOrCreateTwin，上限 16，end_frame 销毁）；同一帧后续写入与最终 temp→compose 合成都走 twin 纹理（text/消息窗 CPU 开销落入 GL quad）。跨帧持久位图（背景/立绘）仍走版本缓存上传。


### P21: v1.2 — 呈现改回读（修真机 GL 卡死） (2026-09-07, 构建 md5 3e327616)
- 真机带标记复测：探针 100% PASS 后**卡死**（日志停心跳 8、无帧活动）——嫌疑=自建上下文里 `SDL_GL_SwapWindow`（模拟器探针 FAIL 从未走到该路径；E-mote 在真机只证明过 FBO+quad+ReadPixels，swap 未经验证）。
- v1.2：删掉 swap 呈现；GPU 合成后 `krkrsdl2_glc_readback()` 把合成 FBO **回读进既有 SDL surface**（ReadPixels 自下而上→逐行翻转复制，32bpp 字节序一致），随后走**原始验证过的上传+RenderCopy+RenderPresent 链路**。风险面只剩"合成+回读"（两者真机已被 E-mote 证明可靠）。
- 呈现包装撤除、SDLBitmapCompletion 恢复始终拷贝（回读后覆盖）。代价：回读 ~10ms + 上传 3.3ms 仍留；收益：compose CPU→GL。


### P22: v1.6→v1.7 — 窗口切换后 glc 全灭根因修复 (2026-09-07, run12 3317c53b → v1.7)
- **真机 run12 破案**：`SDL_CreateWindow` 出现两次（`:76` launcher 窗口、`:1269` 游戏窗口），glc context 建在窗口 1（`gWindow` 在首次 `BeginContext()` 缓存），launcher 窗口销毁后 `SDL_GL_MakeCurrent(死窗口)` 每次失败 → readback 136 次全部死在 `BeginContext()`（`m5 readback ok` 与 `glc-frame.bmp` 从未出现）。这就是所有 glc 构建"只能进软件、进不了游戏"的根因——**游戏窗口上从未有一次成功的 GL 合成**；try_blt 在无有效 context 下 GL 调用空转却返回 true → 引擎跳过 CPU blit → 黑帧/内容丢失。
- **修复 1 — 窗口变化检测与重建**（GLComposite.cpp）：`BeginContext()` 每次经 `TVPGetPrimarySDLRenderer()` 取当前窗口；`gContext && win != gWindow` → `ResetGLState()`（删旧 context、清空 FBO/纹理缓存/twin 表/版本表、重置探针与里程碑）后在新窗口重建。重建后 m1–m5 重新触发，日志可见第二次 `context ready / probe PASS / m5 readback ok` 序列作为重建证据。
- **修复 2 — 帧活性门 `gFrameActive`**：`begin_frame` 开头置 false，仅 BeginContext+探针+FBO 全套成功后置 true；`try_blt` 在门未开时直接返回 false（CPU 路径兜底），杜绝"GPU 空转却吞掉 CPU blit"的黑帧。窗口切换当帧自动退化为纯 CPU 帧。
- **修复 3 — 诊断**：readback 的 `BeginContext()` 失败打印一次 `SDL_GetError()`；快照频率 `%60` → `#1 + %30`，fopen 失败打 `strerror(errno)`。
- 验证：模拟器 glc probe FAIL（readback 截断）自动禁用，本修复不触及模拟器路径；真机复验只需再跑一次带 `gpu-composite.txt` 的 run——预期日志出现第二次 `context ready` + `m5 readback ok` + `frame snapshot saved`，MTP 可拉 `glc-frame.bmp` 做视觉确认。

## 模拟器备注
- Nextendo (Ryujinx fork) 能正常加载/运行 homebrew NRO(已验证 hello.nro 写好 sdmc 文件)。
- 首次启动会弹 prod.keys 提示(homebrew 不需要密钥,点 OK 即可;要消除需把真机导出的 prod.keys 放入 `portable/system/`)。
- 已知模拟器 vs 真机差异:fsdev 的 stat/部分路径操作在模拟器上会挂起(已绕过),真机是最终验证环境。

## 下一步(解锁 M1 的钥匙)
**获得 switch-mesa**(任一途径):
1. 网络可达 pkg.devkitpro.org(换网络/VPN),用 dkp-pacman 装 `switch-mesa`;
2. 或从可达机器拷贝 `libEGL.a/libGLESv2.a/libglapi.a` 等文件;
3. 或离线构建 mesa for switch(Meson 交叉编译,工作量大)。
拿到后替换 stub → 重建 → 模拟器/真机应能显示画面 → M1 达成。
### P23: v1.8→v1.9 — 画面"一点点"根因：shader 预乘 alpha + 字节序 (2026-09-07, v1.8 1eefd01c → v1.9 865f5670)
- **真机 run13**：v1.8（fallback 修复）全部机制生效（m5 打印、快照落盘、fallback 真实计数），但画面仍"只有一点点，其余黑屏"。MTP 拉 `glc-frame.bmp` 程序化统计=**全黑 0% 非黑**（注：快照是 readback #1 早期帧，title_bg 未加载；真正的后期帧须靠新增的非黑量化日志）。
- **根因 1 — shader 无条件预乘 alpha**：`gl_FragColor = vec4(c.rgb * c.a * opa, ...)`。KRKR 位图 alpha 通道常为 0（条带/中间位图）→ GPU quad 输出全黑；而 fold 走 `glTexSubImage2D` 直写纹理不经 shader → 颜色正常。**画面 = fold 的"一点点" + quad 的大片黑**，与症状严丝合缝。
- **根因 2 — 字节序不一致**：KRKR 位图内存 **BGRA**（`b=(v>>0), r=(v>>16)`，blend_function_sse2.cpp:1707 佐证），SDL surface mask 同 BGRA；但 GL 上传/读回全用 `GL_RGBA` → GPU 路径 R/B 互换。
- **v1.9 修复**：
  1. shader 改为**非预乘输出** `vec4(c.rgb * opa, c.a * opa)`；混合函数按引擎语义映射——copy（method 0）= blend off；alpha（method 1）= `(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)`（引擎 TVPAlphaBlend：dst=src.rgb*src.a+dst.rgb*(1-src.a)）；add（method 6）= `(ONE, ONE)`（引擎 TVPAddBlend：纯通道加法不乘 alpha）。
  2. **统一 `KRKRNS_GL_FORMAT = GL_BGRA_EXT`**(0x80E1, llvmpipe 必支持)：上传 TexSubImage2D、fold 直写、ReadPixels 全部 BGRA；probe 红色检查改 B 字节(full[i+2])；BMP 快照写序 B,G,R 修正。
- 验证注意：模拟器 glc probe FAIL 不跑此路径；真机复验看**新日志行 `[glc] frame content: ... nonblack=%.1f%%`**（每 15 次 readback 一行，量化画面完整性，无需拉 BMP）。

### P24: v1.9→v2.0 — 画面"一点点"终局根因：位图 bottom-up 行方向 (2026-09-07, v1.9 865f5670 → v2.0 8dc213e8)
- **run14 量化**：新增 `[glc] frame content: readback #15 nonblack=0.8%` —— 画面确实只有 0.8% 非黑。
- **快照程序化分析（非看图）**：MTP 拉 `glc-frame.bmp`（#15 帧），非黑像素**全部集中在 y=0..7 一个 1280×8 条带**（10240px=1.11%，颜色 BGR=(215,168,102) 为正常内容），其余 712 行全黑。
- **根因——KRKR 位图 bottom-up 存储**：`tTVPBitmap::GetScanLine(l) = (Height-l-1)*PitchBytes + Bits`（LayerBitmapImpl.cpp:505）。`krkrsdl2_glc_get_bitmap_raster` 返回 `GetScanLine(0)` = **逻辑顶行**（内存末行）。而 UploadBitmap/fold 全部用 `pixels + y*pitch` **向内存深处走** → 从顶行再往下就是越界/垃圾 → 纹理只有第一行有数据、其余黑。fold 折叠逐行上传同样错向。
- **v2.0 修复**：行遍历统一改为 `pixels - y*pitch`（逻辑顶行在内存末，向低地址取行）：UploadBitmap 的 repack 路径、fold 单 rect 路径、fold 整幅 repack 路径三处。pitch==w*4 的整块上传天然正确（指针即逻辑顶行，纹理行序=逻辑行序）不动。
- 回读行序审计：GL ReadPixels 返回 bottom-up（行0=FBO底=逻辑底）→ 翻转写 surface（top-down）✓ 与 fold 修复方向自洽；probe 红色检查已在 BGRA 字节序修正（full[i+2]）。

### P25: v2.0→v2.1 — fold 折叠镜像缺失（真机 run15：画面从"顶部一条带"变"底部一条"）(2026-09-07, v2.0 8dc213e8 → v2.1 487ba103)
- **run15 观察**：用户反馈"最底下一点点非黑像素"+闪退。日志 readback 停在 #3、compose-stats 全缺（合成在早期终止）；尾部 TJS 异常 `bgm.tjs playBuffer`/`sysvoice.tjs start`（`syn_00020.ogg` 播放失败，wuvorbis 不可用）——异常会弹错误框，体感"闪退"，属音频问题另案。
- **根因（方向链闭合）**：
  1. render-to-texture 时 GL 把 framebuffer 行 0 放纹理行 0，而 quad 顶点 y0=1-dstY/th*2 使逻辑顶行落在 framebuffer 顶部（=纹理行 H-1）→ quad 内容经 ReadPixels bottom-up + 读回翻转后**恰好正确**；
  2. fold 直写纹理用 `TexSubImage2D(y = r[1]+row)` → 纹理行 K 出现在 surface 行 H-1-K → **上下颠倒**。v1.9 快照"顶部 y=0..7 一条带"（内容错位到顶部）、v2.0"底部一点点"（上传方向修好后折叠错位更明显）都是同一镜像缺失的表现。
- **v2.1 修复**：fold 单 rect 路径目标行改 `ch-1-(r[1]+row)`；整幅折叠（>128 rects）逐行镜像（GL 行 t ← 逻辑行 ch-1-t）；加越界防御（r[1]<0 或 r[1]+r[3]>ch 跳过，防 OOB 读=闪退另一嫌疑）。
- 方向链最终态：UploadBitmap 纹理行 t=源逻辑行 t + quad 顶点翻转 + ReadPixels 读回翻转 + fold 行镜像 = 全链路自洽。
- 遗留：音频异常（ogg 播放失败弹错误框）可能导致用户体感闪退；下轮顺带查 wuvorbis/音频失败路径。

### P26: v2.1→v2.2 — 条带缓存冻结修复 + cpuonly A/B 探针 (2026-09-07, v2.1 487ba103 → v2.2 9601cc1f)
- **run16 观察**：闪退消失（v2.1 fold 越界防御 ✓），但 `frame content #15 nonblack=0.5%`。快照程序化分析：非黑 7103px **全部在 y=0..7 顶部一条 1280×8 条带**（暗色，≈黑），其余 719 行全黑。
- **证据链**：`handled=90` 每帧**恒定** + blt src 指针**恒定**（同一 1280×8 条带对象，krkrz draw-pool 行带）→ 90×8=720 恰为全屏 → 这 90 条带本应铺满画面。
- **根因——家用缓存把条带内容冻结**：纹理缓存按 (ptr,w,h,pitch,version) 键。但引擎 hook（LayerBitmapIntf.cpp:1144）的 `bump_version(this)` 只在 `try_blt` 返回 **false** 后执行；GPU 成功路径直接 `return true`，**版本从不 bump** → 条带位图每帧重填新像素，纹理却永远停留在首帧内容 → GPU quad 每次都画第一条带 → 顶部一条、其余黑。GPU 处理率逐帧下滑（3152/8138→6577/29563）正是缓存失效的表象。
- **v2.2 修复**：
  1. **条带重传**：`sh <= 16`（draw-pool 行带特征）时绕过缓存直接 UploadBitmap 重传——每帧 90 次 × 1280×8×4 = 3.7MB/帧上传，可接受；
  2. **mode 4 cpuonly A/B 探针**（`gpu-composite-cpuonly.txt`）：try_blt 全短路返回 false（CPU 路径独占 compose），readback fold 整帧折叠 → 若 mode4 画面正确则 fold/readback 链好、问题在 quad/纹理链；若仍黑则 fold/readback 链本坏。这是把"画面只剩一条带"二分定位的决定性实验；
  3. **blt# 诊断补 xy 坐标**（此前缺失、无法验证条带坐标分布）。
- 验证：真机先跑 mode 1（gpu-composite.txt 不动）看条带重传是否铺满；若仍一条带则删该文件、建 `gpu-composite-cpuonly.txt` 跑 mode 4 二分。

### P27: v2.2→v2.3 — mode4 A/B 决定性证据 + 快照 y 轴修正 (2026-09-07, v2.2 9601cc1f → v2.3 3e266501)
- **run17 (mode 4 cpuonly) 决定性结果**：用户反馈"短暂闪过游戏画面之后又变回大部分黑屏，底部一条非黑像素带"。
  - **"闪过完整游戏画面" = SDLBitmapCompletion 直拷 surface 的 CPU 完整画面**——它只在 readback 覆盖 surface **之前**可见 → **引擎 CPU 合成完好、surface 直拷完好**，问题锁定在 glc 的 fold/ReadPixels 输出链。
  - mode4 数据吻合：handled=0（全 CPU）、fallback 180→270→360（rect 数随帧增长）、frame content #15=0.0% #30=0.1% → **fold 几乎没把内容写进最终 surface**。
- **快照 y 轴修正（重要认知纠错）**：BMP 规范是 bottom-up（文件第一行=图像底部），此前快照代码按 surface 顺序直写 → 实际存的是上下颠倒图 → 我程序化分析"y=0..7 顶部一条带"实为**屏幕底部** —— 与用户历次描述（"最底下一点点""底部一条带"）完全一致！v2.3 修正 BMP 写序（先写 h-1 行），分析轴与屏幕一致。
- **v2.3 诊断增强**：blt# 诊断移到 mode-4 短路之前（mode4 也打 xy 坐标）；fold 增加 **x 轴越界防御**（此前仅 y，r[0]+r[2]>cw 可越界读→mode1 闪退新嫌疑）；fold 打印前 8 个 rect 的 xy 尺寸 + 总 rect/skipped 统计（确认是否大量 rect 被边界检查过滤——若大部分被 skip 则"底部一条带"= 只有最后一个 rect 通过了检查）。
- 待观察：下轮 mode4 日志的 blt xy 分布（条带 y 是否 0..712 全覆盖）、fold rect 前 8 条、skipped 计数。

### P28: v2.3→v2.4 — mode4 第二阶段 A/B：完全绕开 GL 直拷 (2026-09-07, v2.3 3e266501 → v2.4 f54f3341)
- **run18（mode 4）用户反馈**："短暂出现游戏开头的视频 → 变回底部花屏条带+黑屏；但按按钮能看到按钮单独渲染出来，游戏逻辑在跑"。
  - "视频闪过/按钮单独渲染" = SDLBitmapCompletion 直拷 surface 的 CPU 画面在 readback 覆盖前可见 + 局部重绘帧可见 → **CPU 合成+surface 直拷+raster 访问器全部完好**；
  - blt# 诊断首次带 xy：前 6 条全部 `xy=0,0 sz=1280x8`（条带 blt，mode 4 全 CPU）；fold rect/skipped 日志缺位 → 因 fallback=180/270/360 全部 >128 走**整幅折叠分支**（该分支无日志）——修正认知：并非"大量 rect 被跳过"，而是整幅折叠路径本身。
- **v2.4 决定性实验**：`gMode == 4` 时 readback **完全不碰 GL**——直接把 compose 位图光栅按 bottom-up 行序（cpix - (ch-1-y)*cpitch）memcpy 进 surface，并打 nonblack 量化。**若直拷画面完整 → 锁定 GL fold/ReadPixels 链路为唯一坏环节**（下一步集中修 GL 环节）；若仍黑 → raster 访问器或行序还有问题（可能性低，因 SDL 直拷路径好）。
- 附带：mode 4 的 readback 不再依赖 FBO/GL，探测成本更低；日志新增 `mode4-direct copy: ... nonblack=%.1f%%`。

### P29: v2.4 真机里程碑 — 画面首次完全正常 + glc 黑屏机制定案 (2026-09-07, v2.4 f54f3341)
- 用户确认 run19 游戏画面正常（mode 4）。日志 `mode4-direct: raster unavailable` 贯穿：直拷分支因 `get_bitmap_raster` 返回 null 从未写 surface → 屏幕=SDLBitmapCompletion CPU 直拷 → 画面正常。**并非直拷成功，而是 glc 未干扰**。
- **黑屏机制定案**：surface 原始内容全程正确（视频/按钮/完整画面临现为证）；坏环=glc readback 覆盖 surface（fold+ReadPixels 输出几乎全黑）。解释 v1.7→v2.3 所有"一条带/全黑"现象。
- 下轮焦点：readback 时 `krkrsdl2_glc_get_bitmap_raster(gComposeBitmap)` 为何 null（blt 诊断却 destIsCompose=1）→ 查明后修复 fold 链，mode 1 GPU 合成才有意义。

### P30: v2.4→v2.5 — 回归 GL_RGBA 全链（BGRA_EXT 上传被拒嫌疑）+ GL 错误探针 (2026-09-07, v2.4 f54f3341 → v2.5 9620167d)
- **背景**：用户反馈"游戏正常但太卡"——当前"正常画面"实为 CPU 直拷（mode4 直拷因 raster null 未覆盖 surface），GPU 合成（mode 1）从未成功，性能目标未达成。
- **新嫌疑（解释 probe 绿/quad 黑的矛盾）**：probe 用 `glClear`（无纹理）→ PASS；mode 1 的 quad 用 `TexSubImage2D(GL_BGRA_EXT)` 上传——若 llvmpipe 拒绝 BGRA_EXT（INVALID_ENUM）→ **纹理上传失败→纹理全空→quad 采样黑→全屏黑**，fold 同因。而 E-mote 后端（真机验证过 FBO+quad+ReadPixels 全链路）**只用 GL_RGBA**。
- **v2.5**：
  1. `KRKRNS_GL_FORMAT` 改回 **GL_RGBA**；shader 输出改 `vec4(c.b, c.g, c.r, c.a)*opa`（BGRA 内存上传为 RGBA 后采样通道互换，shader 换回）；
  2. readback 的 ReadPixels→surface 拷贝加 **R/B 字节交换**（GL_RGBA 字节序 → SDL BGRA）；probe 红色检查改回 `full[i]`（RGBA 的 R 字节）；
  3. **GL 错误探针**：proc 表加 `GetError`，GLErr() 埋点于 UploadBitmap/DrawQuad/fold/ReadPixels（每会话记录前 6 个错误码）；
  4. mode4 直拷分支打印 `gComposeBitmap=%p`（解 raster null 之谜：blt 诊断 destIsCompose=1 与 readback raster null 并存）。
- 待真机 verdict：`[glc] GLerr` 是否出现 0x502（INVALID_ENUM）→ 验证 BGRA 嫌疑；mode4 的 gComposeBitmap 指针是否 null。

### P31: v2.5→v2.5b — shader // 注释吞代码导致 run20 闪退 (2026-09-07, v2.5 9620167d → v2.5b a9ad6c19)
- **run20（v2.5 mode 1）**：`[glc] shader compile failed: 0:1(129): error: syntax error, unexpected end of file` + 闪退。
- **根因（我引入的 bug）**：v2.5 在 gFrag 字符串里加了两行 `//` 注释说明——C++ 相邻字符串字面量拼接后 GLSL 源码是**一整行**，`//` 把行尾到 `}` 的全部代码（gl_FragColor 等）注释掉 → shader 编译失败 → EnsureProgram/BeginContext 链崩溃 → 闪退。
- **教训**：GLSL 字符串常量禁止 `//`（单行注释在拼接后无换行保护）；跨行注释必须用 `/* */` 或干脆不放注释。后续 shader 改动必须在构建后用 GLSL 编译验证（真机日志 shader compile failed 是最早信号）。
- **v2.5b**：去掉 gFrag 内 `//` 注释；顺带修正非预乘语义——opa 只乘 alpha（`vec4(c.b, c.g, c.r, c.a * opa)`），RGB 不乘（渐隐不再变暗）。

### P32: run21 架构级根因 — gComposeBitmap 不是全屏合成目标 (2026-09-07, v2.5b a9ad6c19)
- **run21 (v2.5b mode 1)**：shader 修复生效（m1/probe PASS/m4/m5 全过），GLerr 探针 0 条（GL_RGBA 链无 GL 错误，BGRA_EXT 嫌疑排除）；但画面仍"一条带"且闪退。
- **决定性日志**：`fold rect#... (cw=1280 ch=8)` —— `krkrsdl2_glc_get_bitmap_raster(gComposeBitmap)` 返回的位图**只有 8 行高**！
- **架构真相**：`LayerManager::GetDrawTargetBitmap(rect)` 按**需求 rect** 创建/复用 DrawBuffer（LayerManager.cpp:81-117），全屏合成在 `InternalComplete2` 被拆成 8 行条带逐条 `Draw`（LayerIntf.cpp:5947）→ DrawBuffer 实际是 8 行高的临时条带目标。**真正的全屏画面**在 `BitmapLayerTreeOwner::NotifyBitmapCompleted` 逐层拷入 `BitmapNI`（完整主位图，BitmapLayerTreeOwner.cpp:96-175），SDL 直拷的就是它。
- **结论**：glc 从 v1.0 起把"8 行条带 DrawBuffer"当作 1280×720 全屏 compose 目标 → 折叠内容只进 8 行纹理 → 屏幕底部一条带、其余 glClear 黑。**这就是所有"一条带/全黑"现象的终极根源**，与 fold 方向/字节序/缓存等次生问题叠加。
- **正确方向**：GPU 合成目标=BitmapNI（全屏），hook 点=DrawCompleted→NotifyBitmapCompleted（层结果+坐标+混合类型+透明度），而非 Blt（条带/子层内部混合）。readback 覆盖 surface 仍须只在拿到完整主位图时执行。
- **闪退**：可能是 8 行 FBO + quad 越界（条带 dest 坐标 > 8 行高时 Viewport/裁剪）或该帧 fold 目标行越界——下轮把 compose target 换到 BitmapNI 后一并消除。

### P33: v2.6 — layer-composite 重写：hook 移到 NotifyBitmapCompleted（架构修正落地）(2026-09-07, v2.5b a9ad6c19 → v2.6 e3b0f719)
- 按 P32 架构根因落地：**弃用 Blt/DrawBuffer 拦截（8 行条带目标），GPU 合成改走 `BasicDrawDevice::NotifyBitmapCompleted`**——引擎呈现每层最终结果（bits+宽高+行序+cliprect+x/y+type+opacity）的唯一路径，且与 SDL 直拷共用同一语义。
- **glc_layer()**（GLComposite.cpp）：把层位图按行序（bottom-up 时逻辑顶在内存末）上传为纹理（GL_RGBA，与 E-mote 同款已验证路径），以 cliprect 为源、(x,y) 为目标、type/opacity 为混合画 quad 到**全屏 1280×720 FBO**。混合映射：ltOpaque=copy；ltAlpha/ltAddAlpha（type 2/12）=标准 alpha；ltAdditive（3）/ltAddAlpha=add。
- **mode 1 语义切换**：try_blt 直接 decline（CPU 合成继续充当地面真值/纹理缓存），readback 的 mode 1 分支=纯 FBO→surface 读回（R/B 字节交换，nonblack 量化 + layers 计数）。gLayerCount 每帧 begin_frame 重置，纹理缓存上限 256 防泄漏。
- 引擎侧 hook：BasicDrawDevice.cpp include GLCompositeBridge + `<cstdlib>`，NotifyBitmapCompleted 开头取 `TVPBITMAPINFO`（sdl2 类型）调 glc_layer（bottomup=biHeight>0）。
- 预期：mode1 下 FBO 含完整图层合成 → readback 出完整画面；若颜色/方向/混合有偏差，nonblack 与快照可分辨。

### P34: v2.6→v2.6b — layer 发布门控（修 launcher 黑屏）(2026-09-07, v2.6 e3b0f719 → v2.6b 146a3756)
- **run22（v2.6 mode1）**：layer hook 生效（`readback #1 layers=1`，launcher），但**整个软件黑屏**——readback 无条件把 FBO（launcher 仅 1 层、nonblack=0.4%）发布到 surface，覆盖了 CPU 直拷的完整画面。
- **v2.6b 门控**：mode1 readback 先读 FBO、统计 nonblack 与 layers，**仅当 `gLayerCount>=3 && nonblack>15%` 才发布到 surface**，否则 `return false`（保留 CPU 合成画面）。日志标注 `-> PUBLISH` / `-> keep CPU`。
- 本版=观察+安全接管：GPU 合成正确性通过日志量化持续可见（nonblack/layers），任何时刻画面都不会被空 FBO 覆盖黑。

### P35: v2.6b→v2.6c — PUBLISH 快照 + 层流诊断 (2026-09-07, v2.6b 146a3756 → v2.6c bc32beea)
- **run23 (v2.6b mode1) 用户反馈："进游戏满屏花屏，但隐约能看见画面"——重大进展**：GPU 合成真正激活！launcher `layers=1 keep CPU`（门控✓不黑屏），进游戏 `readback #1 nonblack=67.3% layers=90 -> PUBLISH`、#15=30.2%、#30=33.8%——满屏内容由 GPU 合成（不再是全黑/一条带）。
- **问题收敛到视觉细节**：满屏花屏但隐约可见 = 内容画出来了，但颜色/混合/层序有偏差。
- **v2.6c 诊断**：PUBLISH 帧保存 `glc-layer.bmp`（修正写序）+ 首帧 layer#1 的 (xy,clip,尺寸,type,opa) 日志，用于像素级定位花屏形态（字节序/混合/顺序）。
- 已知候选：①层位图像素是 B,G,R,A 内存序 → GL_RGBA 上传 → shader c.bgr 换回（已做）；②alpha 混合 SRC_ALPHA（非预乘，引擎同语义）；③层序=通知序（引擎合成序）；④readback R/B 交换（已做）。快照将一锤定音。

### P36: v2.6d→v2.6e — bottomup 条带上传越界读 → 8 行纹波修复 (2026-09-07, v2.6d 0559fddc → v2.6e d1aecd56)
- **run24/25 快照+诊断定案**：画面 = 8 行纹波（每 8 行完全重复同图案）。layer#2-6 诊断揭示引擎按 **8 行条带**逐条通知（`clip=0,0 1280x8 tex=1280x8`，`xy=0,8/0,16/0,24/0,32` 递增）——每条带是独立 1280×8 位图。
- **Bug**：glc_layer 的 `pitch==w*4` 分支对 bottomup 位图整块上传——纹理行 0 取 `bits+(h-1)*pitch`（逻辑顶在内存末尾）后**连续读 h 行 = 越界**（读到条带位图外/相邻位图）→ 所有条带采样到相同内存 → 8 行纹波满屏。
- **v2.6e**：两个分支统一**逐行重排**（bottomup 逻辑行 r 在 `bits+(h-1-r)*pitch`），与 UploadBitmap 同款；80 行斜纹 → 期待完整拼合画面。
- run25 附带：readback #15 nonblack=95.2%（接近全屏内容）→ GPU 合成内容已接近完整，修复后画面应正确。

### P37: v2.6e→v2.7 — mode5 gpuonly：跳过 CPU 侧合成（双重合成 → 单次合成）(2026-09-07, v2.7 构建中)
- **用户反馈**：v2.6e 画面正常（条带拼合确认 ✓）但「比之前更卡」。
- **根因（量化）**：mode1 = **双重合成**。try_blt 全量拒绝（GLComposite.cpp:672）→ 引擎 CPU 合成 90 次条带照跑；NotifyBitmapCompleted hook 又把这些条带**同时**送给 GPU quad（90 次纹理上传 + 90 quad）和 CPU surface blt（BasicDrawDevice.cpp:710-722）；末了 readback 全屏 + 逐像素 R/B 交换 + SDL 侧照旧全幅纹理上传。GPU 路径完全叠加在 CPU 路径之上 → 必然更慢。
- **v2.7 mode5（gpu-composite-gpuonly.txt）**：
  - try_blt: mode1+5 均拒绝（CPU 主合成仍权威，防纹理缓存依赖）。
  - BasicDrawDevice::NotifyBitmapCompleted：mode5 下**跳过 CPU 侧条带 blt**（`!!krkrsdl2_glc_gpuonly()` gate）——GPU quad 成为唯一画面源（Phase 3 性能目标验证版）。
  - readback gate 放宽：mode5 `gLayerCount>=1`（mode1 保持 >=3），launcher 单层也可发布。
  - readback swap：逐字节 → **uint32 寄存器旋转**（BGRA 内存序目标），省 3.7MB 逐像素循环。
  - readback 计时：%15 帧 log `rb=%.1fms`（SDL_GetTicks）。
- **A/B 判据**：mode5 画面正确 ⇒ CPU 合成可整体移除 ⇒ 下一步呈现链直连 FBO 纹理（杀掉 readback+全幅上传）；模式黑/错 ⇒ CPU 侧合成是 bits 依赖 ⇒ 需 Kirikiroid2 式彻底重写。mode1 保留可回退。

### P38: v2.7→v2.8 — mode5 呈现直连 FBO（消灭 readback+全幅上传）(2026-09-08, 构建中)
- **真机验证（用户确认"游戏画面正常"）**：mode5（CPU 合成完全跳过、GPU quad 唯一画面源）画面正确 → **A/B 判据通过：CPU 合成可整体移除**。v2.7 日志证据：`marker found mode=5` + `readback #1 ... layers=1 rb=13.0ms -> PUBLISH`（launcher 单层 gate 放宽生效）。
- **v2.8**：mode5 呈现链从「ReadPixels→surface→逐像素 swap→全幅 UpdateTexture→RenderClear→RenderCopy」改为 **glBlitFramebuffer(FBO→窗口默认帧缓冲)**：
  - `GLProcs`/LOAD 表新增 `BlitFramebuffer`（ES3 proc，真机 Mesa 3.2 提供；模拟器探针 FAIL 自动关，不受影响）。
  - readback mode5 分支：blit 全幅（1280x720 → drawable size，dock 模式 LINEAR 缩放），返回 true 表示 GPU 呈现完成。
  - SDLApplication：`gpuPresented` 标记（声明在 __SWITCH__ guard 之外，非 Switch 恒 false）→ 跳过 upload + RenderClear/Copy，`SDL_RenderPresent` 只 swap；gate 不过的兜底帧走原 SDL 链（残留旧 surface）。
  - blit 无需垂直翻转（FBO 与窗口同 GL 坐标系，readback 的 flip 已证明引擎顶行=FBO 顶行）。
- **预期收益**：每帧砍掉 2×3.7MB CPU 往返（ReadPixels+swap 13ms 级 + upload 3.29ms）+ RenderCopy 全幅。llvmpipe 只剩：条带上传+quad 合成+1 次 blit+swap → 对白 60fps / E-mote 30fps 目标路径打通。
- **风险**：SDL renderer context 与 GLC 独立 context 共享窗口 backbuffer（blit 在 GLC context、swap 在 SDL context）——若真机出现撕裂/黑帧，观察兜底逻辑（gate 不过→SDL 链）是否自动恢复。

### P39: v2.8→v2.8b — 黑闪修复 + 统计 ReadPixels 移除（动画帧率定位）(2026-09-08, v2.8b 7739d41d)
- **run 反馈**：「已经没有完整动画展示了，一会黑屏一下」。
- **日志定案**（用户粘贴 00:29 日志）：① `[prof] ... total=93.6ms/f fps=10.7 compose=102.06`——E-mote/动画场景引擎 CPU 合成 102ms 是帧率主因（与 v2.5 时代 E-mote 95-145ms 吻合）；② PUBLISH（99.7%）与 keep CPU（0.2%）以 15 帧间隔交替——**黑闪机制**：gate（nonblack>15%）失败帧走 SDL 链显示**空 surface 黑屏**（mode5 的 CPU 侧已跳过，surface 从未写入）；③ rb=13-16ms = **统计用全屏 ReadPixels 每帧都在跑**（v2.8 没省掉的纯开销）。
- **v2.8b 修复**：
  - mode5 **不再做统计 ReadPixels**（省 13-16ms/帧）；gate 放宽为 `gLayerCount>=1` 即 blit——近空但引擎合法的帧（转场/动画间隙）blit 真实内容，不再闪现 SDL 空 surface（黑闪消失）。
  - glc_layer 计时 `layersMs=`（每帧上传+quad 合计，%15 帧日志）——定位条带开销。
  - mode1 原逻辑不变（保统计+gate+surface 复制）。
- 遗留：compose 102ms（引擎 DrawBuffer CPU 合成）→ Stage 3.2（引擎合成 GPU 化/E-mote 图层直通）。

### P40: v2.8b→v2.8c — launcher 黑屏修复尝试：显式 glFlush + FBO 中心探针 (2026-09-08, 16537cfb)
- **run 反馈**：「软件都打不开了，直接黑屏」（v2.8b）。日志：launcher `layer#1` 通知正常、`-> PUBLISH`、心跳 OK——**画面没上屏**。
- **差异定位**：v2.8（能进游戏）与 v2.8b（黑屏）launcher 首帧的唯一代码差异 = 13ms 的统计 ReadPixels 被移除。ReadPixels 有**隐式管线 flush** 副作用——软件驱动上直接 blit 可能读到未落地的 quad。**v2.8c**：blit 前后显式 `gl.Flush()`（新增 Flush proc）+ **一次性 FBO 中心 32×32 探针**（`[glc] blit verify: FBO center nonblack=X.X%`）区分「合成黑」与「上屏黑」；探针失败自动降级 SDL 链。
- **判据**：launcher 恢复显示 → flush 命中；仍黑 → 看探针行：FBO 非黑=上屏链问题（blit/swap），FBO 黑=合成问题（glc_layer）。

### P41: v2.8d→v2.8f — 呈现链三连修：shader 二次交换 / 空队列 Present / 窗口重建重验 (2026-09-08, 6de746c8)
- **run 反馈**：启动页黑屏（但可点击进游戏）；游戏画面「窗口大小错 + RGB 通道错 + 黑闪一直在」。
- **证据**：`present verify: WINDOW center nonblack=100.0%`（backbuffer 有内容）但屏幕黑 → **帧画进了缓冲、swap 后不可见** = SDL_RenderPresent 空队列在软件驱动上的 swap 语义不可靠。
- **根因三件套**：
  1. **RGB 通道错**：呈现 quad 复用合成 shader（`gl_FragColor = vec4(c.b, g, r, ...)` 无条件 R/B 交换）→ gComposeTex 已是显示序内容，被二次交换。
  2. **黑屏**：Present 空队列时 swap 不执行/被跳过 → 已验证的 backbuffer 帧不上屏。
  3. **窗口重建**：game 窗口切换后 verify 不重跑。
- **v2.8f**：
  1. shader 加 `uniform int swapRGB`（条带/合成路径=1，呈现 quad=0），DrawQuad 加参数。
  2. 呈现后 **SDL_GL_SwapWindow(gWindow)** 直换（无条件 eglSwapBuffers），SDL 侧 gpuPresented 帧跳过 SDL_RenderPresent。
  3. ResetGLState 清 gBlitChecked → 窗口重建后重新 verify（game 窗口的 dw/dh 与内容可见性数据）。
- **判据**：launcher 全屏显示、游戏画面色彩正确、黑闪消失；日志第二行 verify（若 gWindow 重建）确认 game 窗口缓冲内容。

### P42: v2.8f→v2.8g — 黑闪根治（gate 移除+保留帧呈现）+ 呈现尺寸改 renderer 输出 + 5 点探针 (2026-09-08, 0920a252)
- **run 反馈**：launcher 可见了（v2.8f SDL_GL_SwapWindow 生效 ✓）；但「画面大小还是不对」「黑闪依旧」「视频播放=黑屏」。
- **v2.8g 修复**：
  1. **黑闪根治**：mode5 移除 layers>=1 gate——readback 每次被调用都 quad+swap。引擎无通知帧（未重绘）时 FBO 保留上帧 → 重显示上帧（画面不变），不再走 SDL 链 Clear+Copy 显示黑 surface。
  2. **尺寸**：呈现目标尺寸改 `SDL_GetRendererOutputSize`（renderer 真实输出=EGL surface；drawable 可能报屏幕分辨率而 surface 更小 → 全屏 quad 被裁剪成 2/3 画面），失败回退 drawable。
  3. **探针升级**：窗口重建后前 3 帧 × 5 点（center/tl/tr/bl/br 8×8）→ `present probe#N size=WxH center=..% tl=..% ...`——直接读出内容在窗口的位置分布（定案「大小不对」是尺寸还是位置问题）。
- **视频黑屏**：视频层走 `AssignMainImage`→普通层通知（VideoOvlImpl.cpp:664-683），理论上应进 GPU 链；待 v2.8g 日志看视频帧的层数/内容分布（若视频帧层数据正常但画面黑 → 视频位图格式问题；若层数异常 → 视频路径未进 GPU 链）。Phase 4 视频为最终改造点。

### P43: v2.8g→v2.8h — 大小根因定案：viewport 残留！+ FBO/窗口双采样探针 (2026-09-08, b8ebc8f1)
- **run 证据**（probe 矩阵立功）：`present probe#1 win=1920x1080 center=100% LL=100% RL=0% LT=0% RT=0%`——内容只在**左下 1280×720** 区域（GL 坐标：左下+中心有，其余角 0）。
- **根因**：呈现 quad 光栅化时 **viewport 仍是合成 FBO 的 1280×720**（从未设为窗口尺寸）→ 全屏 clip quad 被投射到左下 1280×720 区域 → 「画面只有 2/3/在左下」。这就是从 v2.8e 起「窗口大小不对」的物理根因（launcher 与 game 同病）。
- **v2.8h**：呈现前 `gl.Viewport(0,0,dw,dh)`。
- **game 窗口全黑**（帧 846 有 90 层但 probe 全 0%）：探针升级为 **FBO 中心采样 + 窗口四角**（`fbo=..%` vs `LL/RL/LT/RT`）——下一轮日志直接分出：FBO 有内容=呈现链问题（viewport/surface 尺寸），FBO 黑=合成链问题（game 窗口的 glc_layer/引擎）。
- 视频（opmovie.wmv mode=layer）黑屏待同日数据定位（VideoOvlImpl 走 AssignMainImage→普通层通知，理论上应进 GPU 链）。

### P44: v2.8h 真机收官 — GPU 合成链闭环；视频黑屏=Phase 4 null player (2026-09-08, b8ebc8f1)
- **真机确认**：`present probe#1 win=1920x1080 fbo=100% LL=100% RL=100% LT=100% RT=100%`——**四角全内容，窗口大小正常**（用户确认「现在窗口大小正常了」）。viewport 修复（v2.8h）命中，呈现链完整：条带上传→quad 合成→全屏呈现→SDL_GL_SwapWindow 直换。
- **视频黑屏定案（代码级）**：`VideoOvlImpl.cpp:327` Play() 的 __SWITCH__ 分支 = **null player 占位**（注释明说 "until the FFmpeg-backed Switch player is attached"）——Play 立即 SetStatusAsync(Stop)，零帧产出 → 视频层永远黑 → 「播放期间黑屏、播放完出静态画面」完全吻合。**视频=Phase 4 待办**（WA2-ns FFmpeg 管线移植），非 GPU 合成回归。
- **Phase 3 Stage 3.1/3.2 GPU 合成状态**：mode5（gpu-composite-gpuonly.txt）全链工作。性能遗留：compose 段引擎 CPU 合成（对白 13-35ms，动画 102ms）——Stage 3.2 的主战场。

### P45: Phase 4 v1 — FFmpeg 视频播放器 (2026-09-08, e2caf44b)
- **背景**：VideoOvlImpl::Play() 的 Switch 分支是 null player 占位（零帧产出→播放期黑屏）。Phase 4 把 WA2-ns 真机验证的 FFmpeg 管线移植为 iTVPVideoOverlay 实现 `SwitchMovieOverlay`（src/core/visual/sdl2/）。
- **复用**：WA2-ns 编译好的最小静态 FFmpeg 7.1（out/ffmpeg_switch/，同工具链同 CPU）——免去 FFmpeg 编译。
- **管线**（对齐 WA2-ns 全部真机教训）：SDL 解码线程 4MiB 栈；自定义 AVIO（fopen sdmc + fread 循环 + AVSEEK_SIZE）；强制 asf demuxer（跳过探针）；frame_index/fps 节奏（不用流 pts）；sws_scale 在解码线程（SWS_POINT→**BGRA**+负 stride bottom-up 直写引擎 32bpp 位图双缓冲）；事件 SPSC 环形（EC_UPDATE per 帧 / EC_COMPLETE EOF）；停播=quit+join+codec/avio 顺序释放。
- **接入**：VideoOvlImpl::Open/Close/Play/Stop/Pause/Rewind 的 __SWITCH__ 分支创建/驱动播放器并走完整 Bitmap+SetVideoBuffer 流程；krmovie.h 补非 Win32 兼容层（__stdcall/HWND/RECT/BYTE/LONG_PTR/long long、tGetAPI typedef 包 win32）。
- **v1 限制**：无声（音频 v2 待接 TVP 音频链）；Rewind=全量重开（无 seek）。
- **验证点**：真机 OP 视频应显示画面（帧率=解码速度）→ 播放完 EC_COMPLETE → 游戏继续。

### P46: Phase 4 v1.1 — Switch 视频事件桥、双缓冲与线程生命周期修复 (2026-09-08, da68132a)
- **静态审计发现**：P45 虽能编译链接，但 `VideoOvlImpl::WndProc` 的 `EC_UPDATE`/`AssignMainImage` 仍只在 Win32 条件内，Switch 解码帧无人消费；解码器始终写 `buffers_[0]` 却交替发布 0/1；线程被 detach 后 Close 最多等待 1 秒即释放 FFmpeg 上下文，存在并发释放风险。
- **事件链修复**：按 Kirikiroid2 的非 Win32 契约，解码线程经 `NativeEventQueue` 发布一个合并的 `WM_GRAPHNOTIFY`，SDL 主线程排空播放器 SPSC 事件并执行原生 KRKR layer 更新；队列补 `Clear/Deallocate`，对象销毁前移除延迟通知。
- **帧与生命周期修复**：每帧写入 `frame_index & 1` 对应槽后再以 release 语义发布；SDL 解码线程保持 joinable，Stop/Close 先 join 再释放 codec/AVIO/sws；暂停/恢复会修正墙钟，EOF 延迟到末帧时刻后发送 `EC_COMPLETE`，flush 帧也走正常发布路径。
- **兼容补齐**：Switch 初始化 Bitmap 指针；启用 layer visible、帧号/FPS/总时长、原始视频尺寸和视频流数量查询；自定义 AVIO 保存并正确释放 context，seek 增加边界/实际位置校验。
- **诊断**：`[movie] published frame=... slot=...` 证明解码产出；`[video] applied frame=... slot=... layers=...` 证明主线程已把帧交给层；`[movie] complete frames=...` 证明完成事件闭环。
- **本地验证**：`build_nro.sh` 完整编译、链接、打包成功；NRO MD5 `da68132a3b92706c8892f9852356e7e1`。真机画面、方向、颜色、节奏和完成事件待验证；音频仍不在本版范围。

### P47: Phase 4 v1.2 — 视频改走 KRKR 存储流，支持 XP3/自动路径 (2026-09-08, 48e01d47)
- **真机日志定案**：`[opened] opmovie.wmv` 与 `[video] open stream=opmovie.wmv size=25177361` 已证明 KRKR 存储层成功定位视频；紧接着播放器报 `[movie] open failed (fopen) errno=2`，说明失败发生在把存储名二次猜成原生文件路径后。此时尚无 `[movie] ready`、解码线程或发布帧日志，屏幕短暂开头后转黑是 `movie.tjs open` 抛出 `Invalid video size` 的结果，不是渲染链回归。
- **修复**：对齐 KRKRZ Win32 的 IStream 接法，`VideoOvlImpl` 将已由 `TVPCreateStream` 解析成功的同一条 `tTJSBinaryStream` 交给 `SwitchMovieOverlay`；FFmpeg 自定义 AVIO 的 read/seek/size 全部直接调用 KRKR 流，因此松散文件、XP3/7z 条目及 autopath 命中使用同一语义，不再拼 `krkrsdl2_game_dir` 或调用 `fopen`。
- **生命周期与边界**：播放器无条件接管并在 FFmpeg 上下文之后释放存储流；AVIO 缓冲改为规范的 `av_malloc` 所有权；EOF 返回 `AVERROR_EOF`；seek 校验有符号溢出、范围与实际落点；Rewind 重新创建同名 KRKR 存储流。
- **本地验证**：`build_nro.sh` 完整编译、链接、打包成功；唯一产物 `build-switch/krkrsdl2.nro`，大小 26,931,617 字节，MD5 `48e01d47ed27a27ed89bd4969dc7df6f`，SHA-256 `8d8096c73019641af5b93a9db0e318648ca4a8bd39a3f095e65330c1981b8ded`。真机应先出现 `[movie] storage ready` 和 `[movie] ready`；画面、节奏、完成事件仍以真机为准，视频音频仍未实现。

### P48: xgkfg 固定位置闪退定案 — layerExImage 缺失 + 心跳线程退出竞态 (2026-09-09, d9e66768)
- **现象**：星光咖啡馆与死神之蝶（xgkfg）每次到 OP 开场 logo 动画结束的固定位置闪退；真机上进程死亡后大气层 `bsdsocket` 触发 User Break，整机崩溃。模拟器同样复现。
- **模拟器日志定案（干净证据）**：`ServiceAm Exit` 之后，崩溃线程是**我们自己的 heartbeat 线程**——`krkrsdl2_heartbeat_thread → krkrsdl2_logf_impl → write → _write_r → armGetTls` 空指针（`Invalid memory access at 0x0`，TPIDR_EL0 已被 libnx 回收）。heartbeat 是 detached 且 `while(true)`，进程退出后仍继续写日志。真机崩溃报告里 41 个线程停在同一 h264 地址、PC 落在无关代码，是进程死亡后内存被踩坏的次生现象；模拟器符号完整，给出真实落点。
- **触发根因**：游戏 `system/LayerEx.tjs` 用 `Plugins.link("layerExImage.dll")` 向 **Layer 基类**注册 11 个扩展方法（含 `clipAlphaRect`）；Switch 的 `TVPLoadPlugin` 只打 `plugin unavailable` 警告即返回，方法从未注册。`KAGLayer.tjs` 本身不含该成员（字符串表已核对），调用点来自 `world.tjs onPaint → AffineLayer.tjs onPaint`。异常发生在 immediate onPaint 事件内，KAG 无法恢复 → 弹致命错误框（`start.ks 行:31`）→ 进程退出 → 上述心跳竞态崩溃。
- **修复 1（脚本层）**：`compat-patches/system/k2compat.tjs` 增加 `Layer.clipAlphaRect` 垫片。签名 `(dx, dy, src, sx, sy, sw, sh, opa)` 由两处独立证据确定：游戏日志寄存器 dump，以及 `.zcode/affine-image-decompiled.tjs` 的反编译调用点（`clipAlphaRect(maskleft, masktop, tempLayer, 0, 0, imageWidth, imageHeight, 0)`）。实现委托给引擎原生 `Layer.copyRect`（已做 alpha 保留的矩形拷贝），尾部 opacity 接受但忽略。加载顺序已核对：`k2compat.tjs` 在 `LayerEx.tjs` 之前（日志 377 vs 384 行），且 `LayerEx.tjs` 只做 DLL link、不重新定义方法，垫片不会被覆盖。
- **修复 2（C++ 层）**：`krkrsdl2_log_shutdown` 闸门——`krkrsdl2_logf_impl` 首行检查、心跳循环退出条件、`krkrsdl2_cleanup()` 在 `delete Application` 前置位。即使将来还有别的脚本异常，也不会再拖崩进程/系统。
- **模拟器验证**：`clipAlphaRect` 异常 0 次（修复前每帧抛）、消息框 0 个（修复前弹致命框）、OP 动画正常播完（yuzulogo/m2logo）、游戏持续运行至正式对白（heartbeat 持续增长、`main=ok`、音频推进到 `noz001_058.ogg`、50–153 fps）。NRO MD5 `d9e66768fd300475d5487d7867d20e36`，27,843,745 字节。
- **真机待验证**：同样路径应不再闪退，且不再有大气层崩溃报告。

### P49: xgkfg 上半屏变暗修正 — clipAlphaRect 改为原生 alpha 遮罩语义 (2026-09-09, e47db9a8)
- **现象**：P48 修复闪退后，游戏可运行，但画面上约 62% 高度处出现全宽锐利亮度突变（上半屏亮度约为下半屏的 0.22–0.28 倍），像被蒙了一层暗色。
- **根因**：P48 的脚本垫片把 `clipAlphaRect` 实现成了 `Layer.copyRect`——**拷贝 RGB 和 alpha**。而上游权威实现（`.zcode/upstream-krkrsdl3/plugins/LayerExBTOA.cpp`）的语义是**只把源图 alpha 与目标 alpha 相乘**（遮罩），完全不碰 RGB：
  ```c
  unsigned long n = (*p) * (*q);              // dst_alpha * src_alpha
  *p = (unsigned char)((n + (n >> 7)) >> 8);  // 只写 alpha 字节
  ```
  游戏用遮罩图做屏幕遮罩时，垫片把遮罩的黑色 RGB 整块贴了上去，于是被遮罩区域整体变暗。
- **修复**：移除脚本垫片，改为**移植上游原生实现** `krkrsdl2/src/plugins/layerexbtoa/LayerExBTOA.cpp`（`layerExBTOA.dll`，挂 6 个 Layer 扩展方法：clipAlphaRect / fillAlpha / copyAlphaToProvince / fillByProvince / copyRightBlueToLeftAlpha / copyBottomBlueToTopAlpha）。
  - 引擎早已暴露所需属性（`mainImageBuffer` / `mainImageBufferForWrite` / `mainImageBufferPitch` / `provinceImageBuffer*` / `hasImage`，LayerIntf.cpp:9508-9598），无需新增接口。
  - 上游用 `TJS_N`（窄串），本项目 `tjs_char` 是 char16_t，全部改为 `TJS_W`。
  - 静态库链接锚点：`krkrsdl2_link_layerexbtoa_plugin()`（同 emoteplayer 模式），否则注册表静态初始化被链接器丢弃（表现为符号不在最终 ELF）。
  - `PluginImpl.cpp` 加 `layerexbtoa.dll` 分支走 `ncbAutoRegister::LoadModule`（该插件不注册新类，只给 Layer 挂函数，无法用 `TVPHasSwitchBuiltin` 检测）。
- **模拟器验证**：日志 `(info) loaded built-in plugin: layerexbtoa.dll`（不再是 unavailable）；`clipAlphaRect` 异常 0 次；OP 播完进入对白；程序化像素分析确认亮度剖面平整（原 62% 处的 4.5 倍突变消失，仅剩 UI 元素边缘的 ±30）。NRO MD5 `e47db9a81ababade2289ced7713f25b4`。
- **教训**：DLL 插件扩展方法必须按上游实现移植，不能凭签名猜语义；`TJS_N`→`TJS_W` 与静态库链接锚点是本仓库移植 ncbind 插件的两个固定步骤。

### P50: 上传链优化 + 部分更新自检（横纹残留根因坐实）(2026-09-09, 15347831)
- **动机**（真机日志 xgkfg）：稳态帧 8.5ms 花在每帧全幅上传 1920×1080（upMB/f=7.9）而引擎脏区常仅 2016px。
- **改动**：
  1. 剖析器口径修正——`total/fps` 原取单帧间隔冒充每帧均值；现 emit 时用整窗口时间除以帧数（`windowStart` 只在 emit 时推进）。
  2. 上传优化（diff-rows）：surface 与上次上传内容做 shadow 逐行 memcmp，只上传变化行带；shadow 仅在**上传成功后**更新；`InvalidateFullSurface`（纹理/表面重建）清空 shadow 强制全幅；每 60 帧一次全幅兜底。
  3. E-mote GL 后端脏区回读：`Image` 记录本帧绘制包围盒（DrawMesh 顶点 + ClearTarget 全清），`LockTarget` 只回读脏区，未动区保留 CPU mirror（full-frame 时零拷贝语义不变）。
  4. **部分更新自检 `KRKRNS_UploadProbe`**：真实尺寸 1920×1080 纹理 + 三个不同高度 band（红/绿/蓝）+ 间隙行保持灰，RenderCopy 后按 SDL_RenderReadPixels 小 probe 验证。**关键**——2×2 与 1920×2 小探针在模拟器 PASS 但无法复现故障；全尺寸 1080 行探针才暴露问题。
- **横纹残留根因（真机/模拟器 A/B + 探针实证）**：模拟器（Nextendo NV120 软 GL）对**全尺寸纹理的部分行更新**不正确——band 更新后该区域读回为黑/错乱（探针 red/green/blue-band 全 MISMATCH、rgb 读到 0），间隙区保持灰因此画面呈现"横纹残留"；而 full-frame 整幅上传正常（用户 A/B：full-frame 无残留、diff-rows 有）。模拟器 ReadPixels 整幅截断（320 宽）同源——该驱动只可靠支持全幅/小块操作。
- **修复形态**：`fullframe-upload.txt`（强制全幅）/`diffrows-upload.txt`（强制 diff-rows）标记 + **默认运行探针**：BROKEN → 自动 full-frame（正确，稍慢），OK → diff-rows（省带宽）。模拟器探针 BROKEN → 自动 full-frame（画面正常，用户确认）；真机硬件 GL 预期 OK → diff-rows 生效。日志：`[win] upload probe ... partial-update=BROKEN/OK` + `[win] upload mode=...`。
- **模拟器验证**：探针 6 点全打日志；自动回退 full-frame 后画面正常（用户确认无横纹）；NRO MD5 `153478313262f72471d49521a0b6c150`。
- **真机待验证**：探针应为 OK 且 diff-rows 生效（upload 大幅下降）；如真机某驱动也 BROKEN 会同样自动回退，安全。

### P51: Auto Path 表增量构建 — 加载期秒级停顿消除 (2026-09-09)
- **现象**（真机日志 xgkfg）：启动/场景切换时 `(info) Rebuilding Auto Path Table ... Total 97531 file(s) found (220-550ms)` 反复出现几十次，伴随 `MAIN THREAD STALLED for at least 6/9/12/15/18/21 seconds` 一连串，加载要卡数秒到十几秒。
- **根因**（StorageIntf.cpp）：KAG 启动每挂载一个归档就调 `TVPAddAutoPath`，其内部 `TVPClearAutoPathCache()` **清空整个表并置 AutoPathTableInit=false** → 下一次 `TVPGetPlacedPath` 查找触发 `TVPRebuildAutoPathTable()` 全量重建（枚举所有已挂载归档的全部文件名）。挂载 N 个归档 → N 次全量重建，每次 220-550ms。
- **修复**（增量构建）：
  - `TVPAddAutoPath` 只清 `TVPAutoPathCache`（文件名查找缓存，新路径可能遮蔽旧文件，必须失效），**保留表**。
  - 新增 `AutoPathBuiltCount` 记录已入表的路径数；`TVPRebuildAutoPathTable` 在表已建时只枚举 `[BuiltCount..end)` 的新路径**追加**（顺序与全量一致，`tTJSHashTable::Add` 覆盖语义不变），无新增时静默返回。
  - 提取 `TVPEnumerateAutoPathEntry(path)` 单路径枚举，全量/增量共用（archive> 与目录两分支逻辑逐字保留）。
  - `TVPClearAutoPathCache`（compact 回调等）仍全清+重置 BuiltCount。
- **模拟器验证**：Rebuilding 从几十次全量 → 首次一次性 288ms（48018 文件）+ 之后每次 0-10ms 增量；`STALLED` 0 次；游戏正常进入（heartbeat ok）。探针再次确认模拟器部分更新 BROKEN（红色 band 错位写进 green-band 位置）→ 自动 full-frame 保画面正确。

### P52: compose 细分计数（layers/lpxM）— Stage 3.2 立项数据 (2026-09-09)
- `BasicDrawDevice::NotifyBitmapCompleted` 每层一次 `krkrsdl2_prof_accum_layer(cliprect)` → `[prof]` 行新增 `layers=`（每帧层数）与 `lpxM=`（每帧合成百万像素）。
- **模拟器（星光咖啡馆 logo 场景）首轮数据**：`layers=1.0 lpxM≈2.0`（=1920×1080 单层全屏），compose=20-42ms。**结论：单层 1080p 的 CPU 合成本身就 20-42ms**——瓶颈是引擎层内容生成+混合的像素量，不是层数/遍历。真机动画场景 compose 47-54ms 同源。
- **Stage 3.2（图层=GL 纹理、混合走着色器）收益预期**：单层 1080p quad 在 GPU <1ms，compose 可降 20-50×；这是动画场景 30fps 的唯一路径，数据已足够立项。

### P53: 真机启动闪退修复 — 自动上传探针停用，默认回 full-frame (2026-09-09, ed793403)
- **现象**：P50 之后构建真机一打开就闪退，日志停在 `[glc] marker absent (CPU composite) mode=0`（launcher 首次 TickBeat 的上传决策处），模拟器完全正常。
- **根因**：P50 的 `KRKRNS_UploadProbe` 自动探针在**首次渲染前**执行：创建 1920×1080 `SDL_TEXTUREACCESS_TARGET` 纹理 + `SDL_RenderClear/RenderCopy` + **`SDL_RenderReadPixels`**。真机（Mesa 20.1.0-rc3 / nouveau）上该序列崩溃；模拟器跑过掩盖了它。同一驱动之前还表现出整幅 ReadPixels 截断（E-mote 探针 320 宽 bug）——`SDL_RenderReadPixels` 不可靠是已知模式。
- **修复**：上传模式**默认 full-frame**（回归 P50 之前的安全路径），不再自动运行探针；diff-rows 改为显式标记 `diffrows-upload.txt` 开启（可真机验证驱动后使用）。`KRKRNS_UploadProbe` 函数保留但不再自动调用。
- **模拟器验证**：`[win] upload mode=full-frame`，游戏正常（heartbeat ok）。
- **真机待验证**：应能正常打开；如需 diff-rows 性能，放 `diffrows-upload.txt` 后再跑一轮确认无残留/无崩溃。

### P54: 默认启用解码图像缓存 — 点击菜单/对话框卡顿修复 (2026-09-09)
- **现象**（真机，P50-P53 之后仍存在）：点击游戏内各种按钮（系统菜单/快速存取/对话框）瞬间明显卡顿，**某个 CPU 核 100%**。
- **定位**：打开 quickmenu/dialog/file（存档列表）时，主线程**逐个解码 psb 容器内的 TLG 图标**（quickmenu 32 个、dialog 50、**file.pimg 99**）。引擎图像缓存机制（`TVPGraphicCache`：按存储名缓存解码位图，`TVPLoadGraphic` 命中直接拷贝、解码后自动入缓存）**从未启用**——只有脚本 `System.graphicCacheLimit` 属性才调用 `TVPSetGraphicCacheLimit`，本游戏未设置 → `TVPGraphicCacheEnabled=false` → **每次点开菜单都重新解码**。
- **修复**（SysInitImpl.cpp，Switch）：算出 `TVPGraphicCacheSystemLimit`（物理内存/10，上限 512MB）后，若当前 limit==0 则 `TVPSetGraphicCacheLimit(SystemLimit)` 默认启用。日志 `[ns] graphic cache enabled: %lluMB`。游戏脚本仍可显式覆盖。
- **效果**：启动期已加载的 UI 资源解码结果留在缓存 → **再次打开同一菜单/对话框命中缓存、零解码、秒开**；首次打开仍解码（不可避免）。模拟器验证 `graphic cache enabled: 40MB`（模拟器报告内存小；真机 3GB → ~307MB）运行正常。
- **待真机验证**：点系统菜单/快速存取不再卡；若仍有切换场景类卡顿（非 UI 资源），下一步可加 FreeType 字形缓存。

### P55: 图像缓存并发锁 — 修复"点多 UI 后卡死" (2026-09-09)
- **现象**（真机）：P54 启用缓存后反复打开同一 UI 变快，但**连续打开多个不同 UI 后游戏卡死**。
- **根因**：`TVPGraphicCache` 是无锁 `tTJSHashTable`，被**两个线程**访问——主线程同步 `TVPLoadGraphic`/`TVPCheckImageCache`（读写）与**异步图片加载线程**（`GraphicsLoadThread::LoadingThread` 解码后 `TVPPushGraphicCache`/`TVPHasImageCache` 写/读）。启用缓存前这两个异步路径因 `Enabled=false` 从不碰表；启用后并发读写哈希表 → 表结构损坏 → 死循环/卡死。
- **修复**（GraphicsLoaderIntf.cpp）：全局 `std::recursive_mutex gGraphicCacheLock`，包住所有缓存访问点——`TVPCheckGraphicCacheLimit`/`TVPClearGraphicCache`/`TVPPushGraphicCache`/`TVPCheckImageCache`/`TVPHasImageCache`/`TVPLoadGraphic` 查询与写入段/`TVPTouchImages` re-touch/`TVPSetGraphicCacheLimit`。递归锁避免 CheckLimit 在 Push/SetLimit 内重入死锁。
- **模拟器验证**：缓存启用+加锁运行正常（`graphic cache enabled: 40MB`）。同时反复打开同一 UI 保持命中（P54 收益不丢）。
- **待真机验证**：连续点开多个 UI（系统菜单→快存→读档→选项等轮换）不再卡死。

### P56: 选项页「字体选择」卡死根因 — 缺失哑元资源 dummy_colorpicker (2026-09-09)
- **确定性复现**：点击游戏内设置/选项页的「字体选择」项必卡死（画面冻结、仅心跳、无渲染）。**与图像缓存无关**（禁用缓存 A/B 仍卡死）。
- **日志定案**：`kaglayer.tjs loadImages → Layer.loadImages("dummy_colorpicker", 0x1FFFFFFF)` ← `buttonlayer.tjs loadButtons` ← `messagelayer.tjs addSystemButton` ← `hsvcpick.tjs setupColorPicker` ← `option.ks:31`；引擎抛 `Cannot suggest graphics extension for .../dummy_colorpicker` → **致命错误**（弹错误框）→ KAG 状态机停住 → 卡死。
- **根因**：`dummy_colorpicker` 是 hsvcpick（色相/饱和度取色器）的**哑元占位资源名**，该文件在游戏全部归档（play/bgimage/fgimage/main/uipsd/voice/…）与松散目录中**均不存在**；引擎在"无扩展名且 auto-path 无候选"时抛致命异常（PC 版数据含该文件或引擎行为不同）。
- **修复**（数据侧，不动游戏与引擎语义）：在 compat 层补 8×8 全透明哑元图 `compat-patches/system/dummy_colorpicker.png`——`file://?/romfs:/compat/system/` 已在 auto-path（SDLApplication.cpp:4170），`Layer.loadImages` 的 auto-ext-fill 因此命中，不再抛错。构建日志确认 `Writing build-switch/romfs/compat/system/dummy_colorpicker.png to RomFS image`。
- **附带诊断保留**：`Scripts.eval` 3 秒 >2500 次的"eval 风暴哨兵"（打印 `[eval] STORM trace` 脚本栈后中断，防冻结）+ `[imgload] begin/done/dispatch` 异步加载探针 + `no-imagecache.txt`/`no-eval-guard.txt` 标记。
- **待验证**：选项页字体选择项应可正常打开（取色器哑元为透明图，若显示需美化再换图）。

### P57: 游戏内「结束游戏」返回内置启动器（可连续换游戏）(2026-09-11)
- **需求**：游戏内点「结束游戏」后回到软件初始化页面（启动器），可再选别的游戏，而不是退出整个 NRO。
- **机制**：
  1. **拦截退出**（`SysInitImpl.cpp`）：游戏会话中（`krkrsdl2_game_mode`）`TVPTerminateSync`/`TVPTerminateAsync` 不再终止进程，改为置"回启动器"标志；脚本致命异常路径（`ScriptMgnIntf.cpp` 的两处 `TVPTerminateSync(1)`）因此同样被接管。
  2. **窗口关闭延迟判定**（`SysInitImpl.cpp::TVPMainWindowClosed` + `SDLApplication.cpp::krkrsdl2_service_window_close_pending`）：窗口关闭**不等于**游戏结束——launchXP3 的 launcher→game 交接会销毁 launcher 窗口，KAG 也可能重建窗口；因此只记 pending，主循环若在 30 帧内看到窗口重新出现就取消，持续无窗口才判定"游戏已结束"。**（首版直接判定，导致游戏刚进 first.ks 就被打回启动器=“游戏都进不去”）**
  3. **会话清理**（`SDLApplication.cpp::krkrsdl2_return_to_launcher`）：移除本游戏加入的 AutoPath（compat/patch 保留）、恢复挂载前的 `TVPProjectDir/TVPNativeProjectDir/TVPDataPath/TVPNativeDataPath` 与 cwd、清图像缓存/归档缓存/PSB 资源、`TVPReleaseDirectSound`、**显式释放残留的原生窗口**（部分关闭路径只把窗口移出引擎列表，不释放 SDL 窗口 → 重建启动器时报 `Switch only supports one window`），最后重跑 `file://?/romfs:/startup.tjs` 重建启动器。
  4. **TJS 全局清理**（`krkrsdl2/data/sessionglobals_{capture,drop}.tjs`）：脚本引擎跨会话存活，游戏（及 KAG 兼容层）新增的全局会引用上一会话已销毁的对象 → 第二个游戏启动时 `k2compat.tjs makeDummyProperty` / 游戏 `utils.tjs objectHookInjection` 抛 `The object is already invalidated`。首次会话前用 `Dictionary.keys(global)` 记录基准全局集，会话结束时删除新增项。
  5. **KAG boot globals 幂等**（`StorageIntf.cpp`）：`global.inXP3archivePacked` 等在第二次会话被游戏声明为只读，重复赋值抛 "Invalid operation for Read-only or Write-only property" → 改为 `if (typeof(...)=="undefined")` 才赋值。
  6. **E-mote GL 后端窗口重建**（`EmoteGLRenderBackend.cpp`）：后端缓存 window/context，游戏窗口销毁后 `begin()` 永久失败；现改为窗口变化时丢弃旧状态并在新窗口重建 context/program。
- **教训（TJS2 语法）**：**TJS2 不支持 JavaScript 的 `for (key in object)`**（k2compat.tjs 早有注释说明），枚举对象必须用 `Dictionary.keys/values/contains`；C++ 内嵌多行 TJS 易踩语法坑且报错无行号 → 复杂脚本放 `.tjs` 文件执行。
- **状态**：机制与各项修复均已构建部署，待完整流程验证（启动器→游戏A→结束游戏→启动器→游戏B）。

### P57b: 跨会话隔离路线修正 — 放弃"删除全局"，改为引擎级容错 (2026-09-11)
- **走弯路记录**：为消除"上一个游戏的残留对象引用"，先后尝试（a）删除会话新增的全部全局（drop 1070 个）、（b）保留大写类名、（c）按"属性访问抛异常"精准探测失效对象。**三条都不成立**：插件注册的类/常量（`Motion`、`ICC_USEREX_CLASSES`）既不在基准集里、也无法与失效对象区分被删掉；而引擎 `Plugins.link()` 只链接一次、后续会话不重新注册 → 第二个游戏报 `Member "..." does not exist` 或直接闪退。**记录：全局清理不可靠，已完全移除**（`sessionglobals_drop.tjs` 改为空操作，仅保留 capture 供诊断）。
- **最终方案（引擎级容错，通用）**：
  1. `TVPShowScriptException`（`eTJS&` 与 `eTJSScriptError&` 两个重载）在 Switch 上**不再弹致命错误框、不再终止**：记录日志 + **恢复 `TVPSetSystemEventDisabledState(false)`** 后继续执行。缺插件成员、可选资源缺失、跨会话失效引用都由它兜住，任何游戏都不会因此中断。
  2. `Layer.loadImages` 捕获资源不可用异常并跳过该资源（不改引擎底层语义）。
  3. 窗口关闭判定需"本会话出现过窗口"（避免启动交接期误判），窗口释放改为**先摘链再删**并只在 return_to_launcher 里做（launchXP3 中的强删已移除——那是崩溃源）。
- **教训**：跨会话复用一个进程/TJS 环境时，**不要试图删除/猜测对方的全局状态**；正确做法是让引擎对"坏引用"容错。

### P57c: 第二个游戏启动失败/闪退根因 — compat 脚本重复执行导致成员丢失 (2026-09-11)
- **日志定案**：第二次启动时 `Member "Header" does not exist` / `allBitmaps` /（较早的 `Motion`、`ICC_USEREX_CLASSES`）——都是"类已存在但静态成员缺失"。原因：**游戏每次会话都会 `execStorage` 兼容脚本**（`k2compat.tjs`、`win32dialog.tjs`、`k2compat_console/padcommon/modeless.tjs`），它们启动时**重新定义 class**；TJS 对**重复 class 定义抛异常**，脚本**剩余部分（成员绑定/WIN32Dialog.Header 等）不再执行** → 成员缺失 → 后续脚本链崩坏；容错让流程继续跑，最终 native 崩溃（用户看到的"闪退"）。
- **修复**：给 `compat-patches/system/` 下我们自己的脚本（`k2compat.tjs`/`k2compat_console.tjs`/`win32dialog.tjs`）加**每进程一次**守卫（首行 `if (typeof(global.__krkrns_compat_<file>)=="undefined") {` + 末行 `}`）：第一次安装，之后重复 execStorage 直接跳过。前提成立是因为**已移除跨会话删除全局**，首次安装的类/成员在后续会话仍然有效。游戏包内自带的脚本无法修改，但它们重复执行时异常已被容错记录、已有成员保留。
- **附加保护**：`TVPShowScriptException` 内加"错误风暴"判定——3 秒内超过 8 次脚本错误即 `krkrsdl2_request_return_to_launcher()` 结束本会话（在损坏状态下继续执行必崩，回到启动器比闪退好）。

### P57d: 回启动器改为"重启应用"（根治跨会话状态问题）(2026-09-11)
- **结论**：在**同一进程内**复用 TJS 引擎跨游戏不可靠——游戏每次启动都会重新执行自己的初始化脚本（定义 class/全局），重复定义会抛异常并中断其后的成员绑定（`WIN32Dialog.Header`、`ICC_USEREX_CLASSES`、`Motion` 等先后缺失），补丁式容错只能延迟崩溃。
- **实现**（SDLApplication.cpp + SDLEntrypoint.cpp）：`krkrsdl2_return_to_launcher()` 优先 `envSetNextLoad(argv[0])` + `Application->Terminate()` → libnx 链式重启本 NRO，**引擎状态全新且首屏即启动器**；`envHasNextLoad()` 为假时（如模拟器）回退到同进程清理+重跑启动器脚本。
- **保留的通用容错**：脚本错误记录并继续（3 秒 >8 次则结束会话）；`Layer.loadImages` 容忍资源缺失；窗口关闭需"本会话出现过窗口"；compat 脚本每进程一次安装；KAG boot globals 幂等；E-mote GL 后端窗口重建。

### P59: 回退「结束游戏→返回启动器」整个功能（按用户要求）(2026-09-11)
- **决定**：用户明确要求回退到该需求提出之前的版本。P57/P58 的全部功能代码已从工作树中移除并重建部署（nro md5 39d35bbb），完整功能实现已备份在 `D:\KRKR-ns-toolsackup-p58-20260911-152242\working-tree.patch`，将来重启该工作时可从备份恢复。
- **移除内容**：TVPTerminate* 拦截（SysInitImpl.cpp）、主循环回启动器服务与 autocycle 测试脚手架（Application.cpp/SDLApplication.cpp/startup.tjs）、会话全局清理（capture/drop，两份 .tjs 已删除，C++ 版枚举器也已移除）、`krkrsdl2_release_leftover_windows`/`return_to_launcher`、argv 记录与 next-load 重启（SDLEntrypoint.cpp）、插件卸载导出（PluginImpl.cpp）、归档缓存导出（StorageIntf.cpp）、PSB Clear 导出（PsbFilePlugin.cpp）、E-mote 窗口重建 resetForNewWindow、KAG boot globals 幂等化（恢复无条件赋值）、launchXP3 中关于窗口强删的注释、脚本错误"记录并继续"与错误风暴判定（恢复弹框+终止）、execStorage 解析结果日志。
- **保留内容**（本会话该需求之外的修复，均与该功能无关）：P50 性能窗口统计修正+分层上传优化、P51 Auto Path 增量构建、P52 合成层计数器、P53 全帧上传默认+探针禁用、P54 图像缓存默认启用、P55 图像缓存并发锁、P56 dummy_colorpicker 缺资源垫图、Layer.loadImages 容错、eval 风暴守卫、心跳诊断、E-mote 脏区回读优化。
- **行为回到**：游戏内「结束游戏」= 退出整个 NRO（回 hbmenu），与用户提出需求前一致。

### P61: SD 卡目录结构集中化 (2026-09-12)
所有运行时文件集中到 `sdmc:/switch/KRKR-ns/` 一个文件夹下，不再散落在 SD 根目录与 `switch/krkrsdl2/`：
```
KRKR-ns/
├── krkrsdl2.nro        主程序（hbmenu 可列出子目录中的 nro）
├── Game/<游戏目录>/     游戏（原 sdmc:/krkr/）
├── saves/<游戏目录>/    存档（原 switch/krkrsdl2/saves/）
├── patch/system/        用户覆盖层（原 switch/krkrsdl2/patch/）
├── log/                 每次启动一个时间戳日志（保留最新 3 份）
└── *.txt                运行时标记文件
```
- **实现**：新增 `KrkrNSPaths.h`（`KRKRNS_BASE_A/L/U` 三个编码形式的基路径宏，U 版匹配 `TJS_W` 的 `u##X` 编码可与其拼接），全部硬编码路径（SDLApplication 的游戏根/存档/补丁/标记/自带路径候选、GLComposite 的 gpu-composite 标记、SysInitImpl 的 tvpgl/no-imagecache 标记、ScriptMgnIntf 的 no-eval-guard、E-mote 后端选择/半分辨率/调试转储、blt-trace、startup.tjs 的 autocycle）改经该宏拼接。
- **日志迁移**：时间戳日志移入 `KRKR-ns/log/`（启动时自动建目录），裁剪保留最新 3 份，并清理 SD 根目录的旧版固定名日志；`[eval]`/`[eval] result` 每会话上限 200 条（KAG 运行时持续求值，原先是日志体积大头；风暴熔断守卫不受影响）。
- **注意**：旧位置不做自动迁移（游戏可达数 GB）。升级步骤：把旧 `sdmc:/krkr/` 的各游戏目录移到 `KRKR-ns/Game/`、旧 `switch/krkrsdl2/saves/*` 移到 `KRKR-ns/saves/`（保留存档进度）、需要的标记文件移到 `KRKR-ns/`，最后把 nro 放入 `KRKR-ns/` 并删除旧 `switch/krkrsdl2/` 目录。
- **真机验证**：模拟器新布局全流程验证通过（启动器列出新 Game/ 下 2 个游戏、日志/存档/补丁就位）；真机待复验。

### P62: 游戏归档挂载时机调整 — 先执行 patch.tjs 后挂归档 (2026-09-12)
- **背景**：Kirikiroid 生态的启动补丁（游戏目录散置的 `patch.tjs`）会先探测松散文件再决定行为；原顺序在 patch.tjs 执行**前**就把全部游戏归档挂进 auto-path，补丁的探测命中归档内部脚本，导致延迟模块在游戏全局初始化之前执行（LimeLight 启动异常）。
- **实现**：`krkrsdl2_prepare_xp3_game` 只排队归档路径不再立即挂载；新增 `krkrsdl2_mount_xp3_resources()`（SDLApplication.cpp），由 `launchXP3`（StorageIntf.cpp）在执行完 sibling patch.tjs 之后统一挂载（兄弟包→选中包→compat/patch 垫底，P58 的 remove+add 优先级语义原样保留）。日志顺序佐证：`[autopath] at game mount` 现在出现在 patch.tjs 执行之后。

### P63: KAGEX 全屏语义 — Window.fullScreen 控制台恒 true (2026-09-12)
- **现象**：LimeLight 开机白屏——boot 正常推进（UI 树建完、SysCoverLayer 盖屏），first.ks 在游戏自注释 `; wait for full-screen changed` + `@waitstable` 处永等，主循环心跳正常。
- **根因链**：游戏自带 KAGEX `mainwindow.tjs` 的 fullScreen setter 首行 `if (fullScreen == v) return`；我们的原生 getter 返 false → setter 全速执行，在 VM ip=0x464 硬调 `this.getNormalRect()`——该成员仅由 win32 插件 windowEx.dll 注册（全游戏 294 个 tjs 仅此一处调用且**无** typeof 兜底）→ 中途抛异常被游戏 catch，但 `_fullScreenChanging=1` + screenModeChangedTrigger 留在半途 → 开机等待永不完成。Kirikiroid2 能跑=krkrz Android 后端 `GetFullScreenMode() { return true; }`（environ/android/WindowForm.h）使 setter 直接短路，windowEx 在那边同样不加载。
- **修复**（WindowImpl.cpp）：`tTJSNI_Window::GetFullScreen()`（TJS 可见的 `Window.fullScreen` getter）在 `__SWITCH__` 恒返 true；`TVPWindowWindow::GetFullScreenMode()`（FullScreenGuard+引擎内部）保持真实 SDL 状态。
- **⚠ 教训（v1 回归）**：v1 把 GetFullScreenMode 整体翻 true → `tTJSNI_Window::FullScreenGuard()` 开始拦截 TJS 写窗口几何 → **我们自己的** romfs/startup.tjs `launcherWindow.setInnerSize(1280,720)` 抛 `Invalid property in fullscreen` → 启动器主线程 tick=2 起永久 STALLED（NRO "打不开"）。**游戏语义层与引擎 guard 层必须解耦**；另：模拟器跑的就是 Switch NRO（`__SWITCH__` 已定义），不存在"模拟器走桌面分支"。
- **验证**：LimeLight 开机全通（getNormalRect 异常消失、进标题、跑剧情 25fps，用户确认）。

### P64: 内置插件文件存在性探测 — 游戏侧子系统启用判定 (2026-09-12)
- **现象**：LimeLight OP（yuzulogo/m2logo.mtn）与标题背景（title_bg.mtn）加载抛空消息异常（affinesourceimage.tjs loadImages），且 motion.tjs 根本没被加载。
- **根因**：游戏对插件可用性做**文件存在性探测**（`TVPGetPlacedPath` 对 emoteplayer.dll 等做四路径探测 → `[miss]`）→ 内置插件无实体文件 → 探测失败 → 游戏自行禁用 emote 子系统 → .mtn 被当作图片走 `Layer.loadImages` → 失败。Kirikiroid2 靠 APK 私有内置插件仓库（`import-module, plugins`，zeas2 未公开）让探测全部通过。
- **修复**：PluginImpl.cpp 新增 `krkrsdl2_is_builtin_plugin_name()`（emoteplayer/motionplayer/emotedriver/layerexbtoa + `TVPHasSwitchBuiltin` 名单：kagparser/csvparser/psbfile/textrender/wuopus）；StorageIntf.cpp `TVPGetPlacedPath` miss 分支对 `<builtin>.dll` 返回合成路径（`romfs:/compat/system/dummy_colorpicker.png`——`Plugins.link` 对 builtin 走原生注册不读文件）。非内置插件探测保持失败，游戏走自身降级路径。
- **验证**：`loaded built-in plugin: emoteplayer.dll`、motion.tjs/affinesourcemotion.tjs 载入、`[emote] ResourceManager size=1920x1440`、`EmotePlayer.play motion=yuzulogo`，affinesourceimage 异常清零，OP/标题美术恢复。

### P65: KAGEX 方屏扩展画布（exHeight）满屏呈现 (2026-09-12)
- **现象**：LimeLight 画面整体缩到约 75% 宽（左右黑边）且底部一条大黑带（用户截图证实：内容顶对齐、底部 band 全黑）。
- **根因**：游戏 main/config.tjs（明文 UTF-16）`scWidth/scHeight`=可见区 1920×1080、`exHeight`=1440=方屏扩展画布；主层被游戏设为 1920×1440（`[win] SetPaintBoxSize w=1920 h=1440`），底部 360 行由游戏自身 SquareMaskLayer 遮盖；引擎把整个画布按 SDL 逻辑尺寸塞进 16:9 窗口 → 整体 0.75 缩放+黑带。
- **修复**（SDLApplication.cpp TickBeat 呈现分支，`__SWITCH__`）：texture 高于窗口时取顶部 `visibleH = winH*texW/winW` 行（恒等于游戏 scHeight，底座 1:1 / 手持 0.667 同一公式），`SDL_RenderSetLogicalSize(tw, visibleH)` + `RenderCopy(src=顶部可见区)`；画布≤窗口（启动器 1280×720）走原路径不受影响。
- **附带诊断**：TVPScreen.cpp 首次调用打印 `[ns] screen usable bounds`（`System.screenWidth/Height` = `SDL_GetDisplayUsableBounds`，KAGEX 舞台尺寸的依据）。

### P66: 加密 xp3 数据包支持 — Kirikiroid2 xp3filter.tjs 兼容 (2026-09-12)
- **现象**：Riddle Joker（Yuzusoft）点击启动即失败退回启动器：`launchXP3` 抛 `Cannot convert given narrow string to wide string`；日志中 startup.tjs 字节码头非 `TJS2` 魔数（`a64b...`，密文）。
- **根因**：数据包（运行游戏.xp3）内容加密；游戏目录自带 `xp3filter.tjs`——Kirikiroid2 补丁生态的解密过滤器脚本：`Storages.setXP3ArchiveExtractionFilter(function(h,o,b,l){...逐块 XOR...})`。Kirikiroid2 自动把它装入**专用 per-thread tTJS 引擎**并对每个解压块回调解密；本移植无此机制 → 密文原样进引擎当文本编译。
- **实现**（新增 src/core/base/sdl2/XP3ExtractionFilter.cpp，照 Kirikiroid2 src/plugins/xp3filter.cpp 移植）：① `CBinaryAccessor` 字节缓冲对象（`b[i]` 复合赋值运算、`xor/add(start,len,val)`、`ptr`/`count`）；② `XP3FilterRegister` 把游戏回调存入 per-thread 解码引擎（独立 `tTJS` + 专用 Storages 原生类，`thread_local` + 脚本版本号懒重建，脚本不进主引擎=工作线程解压天然线程安全）；③ `TVPXP3ArchiveExtractionFilterWrapper` 挂到 krkrz 树**既有**的逐块过滤钩子（`tTVPXP3ArchiveStream::Read`，info={Offset, Buffer, BufferSize, FileHash}）。launchXP3 在挂载前读 `<游戏目录>/xp3filter.tjs` → `TVPSetXP3FilterScript()`（无文件则清空，其他游戏零影响）。XP3Archive.h 补 `TVPSetXP3FilterScript` 声明与钩子指针 extern。
- **编译坑**：tjs2 无 `TJS_E_BOUNDS`（用 `TJS_E_FAIL`）；同签名成员函数类内声明+定义重复报错。
- **发布**：本批 P62–P66 随 GitHub Release v0.3.1（替换附件，md5 `c5ec1038`）发布。

### P60-R2: 同进程路径重写为「整引擎重建」+ P60b–g 跨会话残留六连修 (2026-09-12，当前版本)
P60 提交后，同进程回退路径由 DeepSeek 重写，随后围绕「第二个游戏异常」累计六处修复。**本节描述的即当前工作区状态**；上一节 P60 的"原地修补活引擎"方案已被取代。

- **核心机制变更（整引擎重建）**：会话结束时不再修补活引擎，只置 `Application->Terminate()` + `krkrsdl2_engine_reinit_wanted`；主循环退出后 `krkrsdl2_reinitialize_engine()`（SDLApplication.cpp）执行：`delete Application`（窗口随之释放）→ 清归档/PSB 缓存 → `TVPUnloadBuiltinPlugins()`（AllUnregist+清注册表，插件类随后由下一局 `Plugins.link` 经 ncbind 自动注册表重装）→ 脚本引擎 uninit 并复位 init/uninit 一次性 latch → E-mote work-layer/kagWindow/渲染后端重置 → 重置会话路径 → `krkrsdl2_create_engine()` 重建。启动器=全新引擎首屏。P58 的"删会话全局/sessionglobals_drop"路线彻底废弃。`tTJSObjectProxy` 增加空 Dispatch 防御（重建后残留 proxy 不再空指针崩）。`process_events()` 在重建挂起时跳过 `TVPSystemUninit()`（atexit/初始化 latch 均单向，二次初始化会坏）。
- **compat 命名空间维护**：compat 三脚本加每进程一次安装守卫（`__krkrns_compat_*`，防重复 class 定义中断脚本）；新增 `compat-patches/system/k2compat_reinstall.tjs`——游戏自带桌面版 `system/k2compat.tjs` 会把 `Krkr2CompatUtils` 换成依赖 k2compat.dll 的空壳，故在 `launchXP3` 游戏启动前与 `execStorage` 检测到任意 k2compat.tjs 执行后各重装一次（`ScriptMgnIntf.cpp`）。
- **P60b（重建清理第一轮）**：① 新增 `krkrsdl2_reset_auto_paths()`（StorageIntf.cpp）——引擎重建时清空 `TVPAutoPathList`+查找表+latch 并重播种 romfs 根（此前上一局归档跨局残留，列表实测 12→69→80→217 只增不减）；② 恢复被重写丢失的 `TVPClearGraphicCache()`（图像缓存按请求名做键且先查缓存后解析路径，上一局同名解码图会喂给第二局）与 `TVPReleaseDirectSound()`；③ `TVPDataPath/NativeDataPath` 恢复开机默认 `file://romfs:/`。
- **P60c（脚手架）**：autocycle 轮次改由原生计数器驱动——新增 `Storages.getAutocycleRound()`（StorageIntf.cpp + `krkrsdl2_autocycle_round_count()`）；旧 TJS 全局计数器在引擎重建后归零，脚手架永远选第一个文件夹、测不到换游戏。
- **P60d（第二局 OP/标题动画消失）**：tzxm 的 patch.tjs（TJS2 字节码，zlib 解包确认含 `Plugins.link("emoteplayer.dll")` + `ResourceManager.setEmotePSBDecryptFunc(...)`）向 E-mote ResourceManager 装 **PSB 解密闭包（inline static，进程级）**；引擎重建后闭包引用的旧引擎对象已死，第二局读 .mtn 时调用即抛异常 → `parsed=0` 静默放弃。修复=`ResourceManager::resetDecryptStateForEngineRestart()` 清闭包+种子（不 Release，借用指针）。方法论：[execStorage] 执行链完全一致 → 对比异常 dump（patch.tjs 异常只在故障局）→ 解包 patch.tjs 找 setter。
- **P60e（第二局画面只剩左 1/4）**：`EmoteGLRenderBackend::begin()` 的窗口指针比较被 **SDL 地址复用**骗过，继续使用绑定在已销毁窗口上的私有 GL context，网格绘制/读回经坏 context 损坏。修复=覆写 `ResetForEngineRestart()` → `impl->resetForNewWindow()`（无条件丢弃，不发 GL 调用）。**教训：重建引擎后所有"指针变了才重置"的缓存都不可信**（GLComposite/SW 后端/GL 后端三处同型陷阱）。
- **P60f（第二局启动卡顿）**：非只读流打开与 `TVPSetCurrentDirectory` 走 `TVPClearStorageCaches` → 连 `TVPAutoPathTable`+增量 latch 一起清 → 第二局启动 7+ 次全量建表（97k 文件 × ~250ms）。修复=新增 `TVPClearAutoPathLookupCache()`（仅清查找 memo），表只随列表变化失效。代价：运行时新建进 auto-path 目录的文件不再被未限定名命中——Switch 上无此场景（存档在 dataPath）。
- **P60g（E-mote 慢 2-3 倍）**：后端选择一次性锁存存在竞态——进程首个 E-mote 调用落在窗口切换空档时 `begin()` 失败 → 整个进程锁死 CPU 后端（模拟器 GL 自检本就不稳定，GL=快、CPU=慢）。修复（TVPCompositor.cpp）：主渲染器不存在=临时状态，本次回 CPU 但**不锁存**，后续调用重试 GL；自检真失败才锁存。附带：`resetForNewWindow()` 保留 `readTileWidth`（渲染器属性=截断读回补偿值，清零会让 GL 局重建后 1/4 宽复发）。
- **验证**：模拟器 tzxm↔星光咖啡馆多轮互换，OP/标题动画、菜单美术、分辨率、启动速度全部正常（用户确认，2026-09-12）。注意 P60d/e/g 仅存在于模拟器同进程重建路径；真机 envSetNextLoad 链式重启每次全新进程不受影响，P60f 对真机亦有普遍收益。
- **交付**：NRO 见 GitHub Releases（md5 `5c928fbc`）。


- **背景**：P57/P58 实现过该功能，P59 按用户要求整体回退。本次重启该工作，**复用 P58 的最终设计**（备份 `D:\KRKR-ns-tools\backup-p58-20260911-152242\working-tree.patch`）并重新落地。
### P60: 重新实现「游戏内退出 → 回到内置启动器」(2026-09-12；同进程路径已被上方 P60-R2 取代，以下为当时记录)
- **背景**：P57/P58 实现过该功能，P59 按用户要求整体回退。本次重启该工作，**复用 P58 的最终设计**（备份 `D:\KRKR-ns-tools\backup-p58-20260911-152242\working-tree.patch`）并重新落地。
- **恢复方式（可复核）**：备份补丁是相对 `HEAD` 的 diff，含已保留的 P50–P56；直接用 `git apply` 会冲突。做法是先把 P50–P56 树提交为基线，再用 `git apply --3way` 复现 P58 状态，两者相减得到**纯功能增量：11 文件 / +644 −20**，确认与 P50–P56 **零重叠**（不会撤销既有修复）后应用到基线。逐文件复核确认 `BasicDrawDevice.cpp`、`KrkrNSProf.h`（P52 合成层计数器）与 `GraphicsLoaderIntf.cpp`（P55 缓存锁）等**未**被改动。
- **核心机制**：
  1. **汇聚**（`SysInitImpl.cpp`）：游戏会话中 `TVPTerminateSync/Async` 与 `TVPMainWindowClosed` 不再终止进程，改为 `krkrsdl2_request_return_to_launcher()`。
  2. **窗口关闭延迟判定**（`SDLApplication.cpp`）：启动器→游戏的交接本身销毁窗口，KAG 也可能重建窗口，所以窗口关闭只是**候选**；需"本会话出现过窗口"+ 持续 30 帧无窗口才判定会话结束。（首版直接判定导致"游戏刚进 first.ks 就被打回启动器"。）
  3. **主循环服务**（`Application.cpp::Run` 顶部）：每帧服务窗口关闭候选 + 取标志 + 执行拆卸。
  4. **两条路径**（`krkrsdl2_return_to_launcher`）：真机 `envHasNextLoad()` → `envSetNextLoad(argv[0])` → `Application->Terminate()` 链式重启 NRO（引擎全新）；模拟器等无 next-load 宿主回退到**同进程拆卸**——释放残留原生窗口（libnx 只允许一个）→ 移除本游戏 AutoPath（compat/patch 保留）→ 还原 ProjectDir/DataPath/cwd → 清图像缓存/归档缓存/PSB/音频 → 重跑 `romfs:/startup.tjs`。
- **跨会话状态**（同进程路径的真正难点，沿用 P58 结论）：
  - **删除本会话新增的全局**（C++ 枚举 `TVPGetScriptDispatch()->EnumMembers` + `DeleteMember`，基准集在挂载首个游戏前采集，跳过 `__krkrns_*`）。P57b 的三条弯路（删全部/保留大写类名/按异常精准探测）均已被证伪，不再重复。
  - **插件卸载次序**：`TVPUnloadBuiltinPlugins()` = `ncbAutoRegister::AllUnregist()` **然后** `TVPRegisteredPlugins.clear()`。**次序是正确性关键**：P58 §2 记录"不要清空 `TVPRegisteredPlugins`"针对的是**只清注册表、类仍在全局**的情形（那样 `Regist()` 必抛 `Already registerd class.`）；先 `AllUnregist()` 把类从全局摘掉、再清注册表，二者才自洽。
  - **脚本错误容错**（`ScriptMgnIntf.cpp`）：`TVPShowScriptException` 两个重载在 Switch 上记录日志 + 恢复事件投递 + 继续执行，不再弹致命框终止；缺插件成员/缺资源/失效引用都由它兜住。3 秒内 >8 次则 `krkrsdl2_request_return_to_launcher()`（损坏状态下继续跑必崩）。
  - **compat 脚本每进程一次安装**（`compat-patches/system/{k2compat,k2compat_console,win32dialog}.tjs`）：首行 `if (typeof(global.__krkrns_compat_<file>) == "undefined") {` + 置位 + 末行 `}`。TJS 对重复 `class` 定义抛异常并**中断其后整段脚本**，静态成员绑定（`WIN32Dialog.Header` 等）因此永不安装 → 第二局启动失败。`k2compat.tjs` 内的 `function Krkr2CompatUtils() {}` 同时改为 `global.Krkr2CompatUtils = function() {};`——块内的函数声明在 TJS2 是块作用域，不会成为全局。
  - **AutoPath 优先级**（`krkrsdl2_prepare_xp3_game`）：compat/patch 路径每局先 `TVPRemoveAutoPath` 再 `TVPAddAutoPath`，保证它们恒在列表末尾。否则第二局时游戏自带的**桌面版** `system/k2compat.tjs` 会反超我们的 Switch 桩（`TVPAddAutoPath` 对已存在路径跳过）。
  - **E-mote GL 后端**（`EmoteGLRenderBackend.cpp`）：`resetForNewWindow()` 在检测到 SDL 窗口变化时丢弃绑定在旧窗口的 context/program，在下一次 `begin()` 重建。
- **测试脚手架**：`sdmc:/switch/krkrsdl2/autocycle.txt` 存在时，游戏约 20 秒自动结束、启动器轮流启动各游戏目录 → 无需 UI 输入即可回归"换游戏"全流程。**该文件平时不存在，行为与正式版一致**（验证完需删除）。

### P58 (已随 P59 回退，P60 已重新落地；以下为当时记录): 第二个游戏启动失败的真正根因 + 修复（2026-09-11）
以 `tkzm`（游戏A）→ 结束 → `【KRKR】星光咖啡馆与死神之蝶`（游戏B）的完整流程在模拟器复现并逐条定位。**P57b/P57c 的结论部分是误判**（当时被第 1 条根因掩盖），现更正如下。

1. **【根因·通用】compat/patch 自动路径优先级被翻转**（`SDLApplication.cpp::krkrsdl2_prepare_xp3_game`）
   - `TVPAutoPathTable` 是**按文件名建的哈希表，后加入者覆盖先加入者**（`tTJSHashTable::Add` 对同 key 直接改值）；而 `TVPAddAutoPath` **对已存在的路径直接跳过**（去重）。
   - 首次会话顺序为 `[游戏归档…, romfs compat, sdmc patch]` → 我们的 `k2compat.tjs`/`win32dialog.tjs`（Switch 桩）胜出。
   - 结束游戏返回启动器后，compat/patch 两个路径**仍在列表里**，第二次 `prepare_xp3_game` 时 `TVPAddAutoPath` 跳过它们 → 新的游戏归档被**追加到其后** → 游戏自带的**桌面版** `system/k2compat.tjs`、`system/win32dialog.tjs` 反而胜出（两款游戏包内都确实带了这两份 UTF-16 脚本），于是引擎加载了依赖 win32 插件的原版脚本 → `Member "Header" does not exist` / `makeDummyProperty` 等连锁失败。**这就是"第一个游戏正常、第二个游戏崩"的机制**（同一游戏作为第一个启动也正常）。
   - **修复**：每次挂载游戏前先 `TVPRemoveAutoPath` 再 `TVPAddAutoPath` 这两个路径，保证它们始终位于列表末尾（最高优先级）。
2. **【根因·通用】上一会话的 TJS 全局残留**（`krkrsdl2/data/sessionglobals_capture.tjs` + `sessionglobals_drop.tjs` 重新启用）
   - 会话结束时按基准集删除本次新增的全局。**实测确认**：`Dictionary` 等引擎类**在基准集内**（日志 `Dictionary is Object, in base=1`），插件类也在基准集内（引擎启动时经 `TVPCauseAtInstallExtensionClass` 注册）→ **不会被删**。删除量约 900 个/次。
   - 不删则第二会话在 KAG `utils.tjs objectHookInjection` 读 `__InjectionTable` 里上一会话留下的 `releaseCapture_` 包装（指向已失效对象）抛 `The object is already invalidated`。
   - 配套：**不要**清空 `TVPRegisteredPlugins`。清了之后 `Plugins.link("emoteplayer.dll")` 会再次 `Regist()`，而类仍在（基准集内）→ 抛 `Already registerd class.`，第二个游戏再次失败。记录保持一致即可。
3. **【通用健壮性】**`startup.tjs::launchSelectedGame` 的 catch 里补 `launching = false`：`launching` 是所有点击/按键的闸门，启动失败抛异常会把它永久卡在 `true` → **启动器看起来"卡死"**（用户实际遇到的现象）。
4. **诊断增强**：`KRKRNS_LOG("[launcher] own path=%s next-load=%d")`（启动时一次，判定能否链式重启）、`[execStorage] 名字 <- 解析结果`（一眼看出 compat 脚本被谁覆盖）。
5. **平台事实（实测）**：模拟器（Nextendo/Ryujinx）**`envHasNextLoad()==0` 且不传 argv**，因此**只能走同进程回退**；真机 hbmenu 提供 argv[0] 且支持 next-load → 走链式重启。`krkrsdl2_return_to_launcher()` 增加了 argv 缺失时对常见安装路径的探测。
6. **测试脚手架**：`sdmc:/switch/krkrsdl2/autocycle.txt` 存在时，运行中的游戏约 20 秒自动结束、启动器自动轮流启动各游戏文件夹 → 无需 UI 输入即可回归"换游戏"全流程。**该文件平时不存在，行为与正式版一致**（验证完已删除）。
- **实测结果（模拟器，带 autocycle）**：连续 5 轮「启动 → 结束 → 回到启动器 → 再启动」的**交接本身已稳定**——每轮都能结束会话、清掉 900 个残留全局、重建启动器并再次启动，无卡死、无 native 崩溃。
- **当时仍存的问题（已修，待复验）**：第 2 轮起 `Plugins.link` 抛 `Already registerd class.`（原因见第 2 条后半：多加了清空插件注册表的动作）。已撤销该动作，第二次会话应能完整启动；再次带 autocycle 复验时确认。
