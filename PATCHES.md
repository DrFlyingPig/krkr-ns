# KRKR-ns 补丁记录 (PATCHES)

> 📋 **开发全过程复盘(需求/流程/踩坑)见 [DEVLOG.md](DEVLOG.md)**——本文件只列代码补丁的技术细节。

相对上游 krkrsdl2 (pinned `bf207f2`) 的全部本地改动。同步上游时逐个重新应用。

## 已应用补丁 (源码层)

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