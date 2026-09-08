# KRKR-ns 开发复盘(2026-09-03 ~ 09-04 会话)

本文档整理本次移植窗口的完整记录:做了什么、实现了哪些需求、开发流程、以及最重要的踩坑清单。配套文档:
- `PORTING_PLAN.md` — 已批准的移植计划(阶段/里程碑/风险)
- `PATCHES.md` — 全部代码层补丁的技术清单(本文踩坑处均有对应补丁条目)

---

## 一、这个窗口都干了什么

### 1. 里程碑收尾
- **M1 正式收官**:显示修复组合拳(构造器纹理创建时机 + 图层显式尺寸 + 逻辑分辨率)在模拟器与真机**双端全屏红确认**。
- **Phase 3 输入完成**:手柄→鼠标/键盘事件合成,模拟器日志与真机按键双重验证(A=蓝右击、B=绿左击,NS 物理键语义)。

### 2. 键位语义修正(用户裁定)
- NS 物理键位与 Xbox 位置语义相反(A 右下、B 下、X 右上、Y 左上)。
- `SDL_CONTROLLER_BUTTON_*` 按 Xbox 位置语义处理,合成动作与 NS 物理键对齐(交换 A/B、X/Y),真机验证通过。

### 3. 产物命名统一(用户要求)
- 构建产物固定为 `build-switch/krkrsdl2.nro`,不再出现 `-RED3/-INPUT7/-patched` 等后缀;模拟器入口固定 `portable/games/krkrsdl2.nro`。

### 4. 多图层支持(Phase 6a)
- 放开 `tTVPBasicDrawDevice` 的单个 LayerManager 限制(上游历史遗留);
- 确认 KRKR 规则:**顶层图层不可移动**("Cannot move primary layer"),**子图层可移动**——KAG 式布局成立。

### 5. 真实游戏加载链(Phase 6b,核心工作量)
用户提供《星光咖啡馆与死神之蝶》(不拷贝、junction 路径映射),模拟器实测打通:
- 11 个 xp3 自动挂载(含中文名,UTF-8 转换)
- 四种存储路径形态全部本地化(`?/`、`file://?/`、`file://sdmc:`、前导 `/`)
- 游戏 startup.tjs(字节码)→ system/Initialize.tjs → KAG 初始化 → 47740 文件索引
- 插件兼容体系:link no-op + 假注册表 + **补丁目录覆盖机制**(sdmc 文本文件覆盖 xp3 内同名脚本,改文件即迭代、无需重建)
- 已连续翻过:win32dialog / k2compat 全家 / conductor / affinesourcemotion(Motion),当前推进到 menus.tjs

### 6. ONScripter NS 版调研(用户建议)
提炼成熟方案:手柄映射表(A=Enter/B=Esc/X=跳过/Y=自动)、字体三层兜底(游戏目录→romfs 内嵌→SD 卡)、每游戏独立存档目录、多游戏浏览器、hid 直轮询触摸,列入 Phase 6c 执行清单。

---

## 二、实现了哪些需求(对照计划)

| 里程碑/阶段 | 内容 | 状态 |
|---|---|---|
| M1 | 引擎启动 + 画面显示 | ✅ 双端全屏红确认 |
| M2 | 手柄输入可用 | ✅ 真机确认(NS 物理键语义) |
| Phase 3 | 输入与交互(光标/点击/键盘合成) | ✅ 完成 |
| Phase 6a | 多图层合绘 | ✅ 放开限制+子图层验证 |
| Phase 6b | 真实游戏加载 | 🟡 启动链推进至 KAG 菜单初始化,插件兼容补丁持续迭代中 |
| Phase 4 | 存档路径 | ⏳ 未开始(已定方案:独立 save 目录) |
| Phase 5 | 字体/音频/性能 | ⏳ 未开始(字体兜底方案已定) |
| Phase 7 | 打包发布 | ⏳ 未开始 |

---

## 三、开发流程(每个问题怎么被解决的)

**核心循环(分钟级)**:
```
改代码 → bash build_nro.sh → 复制 NRO 到模拟器 games/ → 重启模拟器
→ grep 日志(sdmc:/krkrsdl2_debug.log)→ 定位崩溃点 → 修复 → 复测
```
补丁目录阶段后循环更短:**改 sdcard/switch/krkrsdl2/patch/system/*.tjs → 重启模拟器**(无需重建)。

**调试手段栈(按使用频率)**:
1. **fd 日志**(`krkrsdl2_debug.log`):所有启动/存储/输入关键路径打点;`Debug.message`(TJS 侧)在 Switch 上镜像到同一日志——脚本层也能打日志。
2. **异常自动反汇编**:TJS 异常时引擎自动 dump VM 寄存器+最近指令(TJS 内置)——`gpd %1, %-2.*1 "WIN32Dialog"` 这类输出直接揭示崩溃点语义。
3. **hex dump 打印**:`AsStdString` 在 Switch 上输出不可读(见坑 5),调试时用 UTF-16 单元 hex 打印。
4. **xp3 提取器**(临时 Python 工具):读 zlib 索引+segment,提取游戏脚本/字节码做字符串分析("FOUND 名"/方法名 dump)。
5. **真机验证**:由用户拷贝 NRO、反馈现象、MTP 只读拉日志;模拟器可复现的问题优先模拟器解决,键位等环境差异以真机为准。

**验证策略**:模拟器(Nextendo)先行、真机兜底;两环境行为差异(如 stat、chdir、虚拟手柄)都要记录并区分。

---

## 四、踩的坑(最重要,按类别)

### A. 环境/构建类
1. **模拟器加载到旧文件(Games 目录递归扫描)**
   现象:日志出现已删除构建的标记(`BUILD=TEXFIX2-2140`)。
   真相:Nextendo **递归**扫描 `portable/games/`(含 `_old/` 备份子目录),加载了旧 NRO。
   解决:games 目录只留单一入口;备份移到模拟器目录之外;构建标记+md5 双核对。

2. **`mklink /J` 中文目标路径被拼错盘符**(cmd 编码/转义地狱)
   现象:目标变成 `D:\C:\Users\...`。
   解决:改用 PowerShell `New-Item -ItemType Junction`(原生 Unicode,一次成功)。

3. **devkitPro 全局变量过期**(`/opt/devkitpro`)——脚本内强制 `D:/devkitPro`。

### B. 存储/路径类
4. **`chdir()` 在模拟器 fs 层无效**
   现象:相对名解析始终命中 romfs 的 startup.tjs。
   真相:KRKR 相对名规约用**引擎自己的** CurrentDirectory(`TVPSetCurrentDirectory`),与 POSIX cwd 无关。
   解决:游戏模式显式 `TVPSetCurrentDirectory(game_dir)`。

5. **`ExePath()` / `TVPGetAppPath()` 是 `static` 缓存**
   现象:游戏模式切换后 `System.exepath` 仍是 romfs(插件探测路径全错)。
   真相:首次调用(挂载前)=romfs,永久缓存。
   解决:去掉 static,每次动态计算。

6. **fsdev 路径的四种形态都要处理**
   - `?/<dev>`(媒体剥离后)、`file://?/<dev>`(P5 原补丁)、`file://sdmc:/...`(ExePath 派生)、**前导 `/`**(`/sdmc:/...`,规约链产生,fsdev 打不开)。
   曾误判 junction 子目录问题——真凶是前导斜杠(见日志 `exist fopen-FAIL: /sdmc:...`)。

7. **模拟器 `stat()` 挂起/失败**
   现象:存在性检查(插件 dll 等)恒 false。
   解决:`CheckExistentStorage` 在 __SWITCH__ 用 `fopen` 探测替代 `stat`。

8. **中文文件名 Fatal**
   现象:`ttstr(窄字节)` 转码遇到 `启动游戏.xp3` 直接 "Cannot convert given narrow string to wide string" 退出。
   解决:显式 `TVPUtf8ToUtf16` + 单个失败跳过。

### C. 引擎/脚本语义类
9. **`AsStdString()` 返回 `char16_t` 串**(krkrz 定制,非 std::string)
   现象:所有打印变 "?";与 `const char*` 三元混用编译错。
   解决:压测打印用 hex/长度;拼日志避免 char16_t 与 char* 混合。

10. **TJS 细节三连**
    - `int` 没有 `toString(16)`(脚本 `key.toString(16)` 崩)→ 十进制拼接。
    - `tTVPMouseButton` 枚举:`mbLeft=0, mbRight=1, mbMiddle=2`(与 Windows 1/2/4 不同)→ 测试脚本曾用 `==2` 判右键导致 A/B 同色。
    - **`{}` 对象字面量(带成员)在文本模式 Syntax error;`new Object()` 无 `Object` 全局** → 一切"命名空间对象"用函数对象(`function NS(){}` + 挂成员)。

11. **`class X extends 插件类` 在插件缺失时崩溃**
    现象:`WIN32DialogEX extends WIN32Dialog` → "Member WIN32Dialog does not exist"。
    分析链:脚本内 `var WIN32Dialog = undefined` 遮蔽全局 stub → TJS 对 void 值成员报 Member not exist。
    解决:补丁目录同名覆盖脚本(文本版),补丁内定义可 extends 的 TJS class。

12. **KAG 的 `Plugins.isAvailable` 决定脚本加载分支**
    现象:isAvailable=false 时 `system/motion.tjs` 不被加载 → `Motion` 类缺失连锁崩溃。
    解决:假注册表(link 记录、getList 报告已加载)让 isAvailable=true,配合补丁类 stub。

13. **字节码脚本(TJS2100)无法文本替换** → 补丁目录覆盖(文本版优先于 xp3 字节码)。

### D. 渲染/输入类
14. **BasicDrawDevice 单 LayerManager 限制**(D3D 时代遗留)
    现象:第二个顶层 Layer 直接 Internal Error。
    解决:放开检查(合成路径本身支持多管理器)+ 顶层不可移动改子图层。

15. **模拟器虚拟手柄不可靠**
    - 键盘映射与 Config.json 不符(z→B、c→Y);8 个手柄按钮位漂移(同键时报 B 时报 DpadRight)。
    - 结论:模拟器仅验证"链路通断",键位语义以真机为准。

### E. 工具/流程类
16. **Edit 工具与外部修改冲突**("File modified since read" 反复);`#if 0` 块平衡被误破坏 → 小步编辑或 python 原子替换。
17. **bash heredoc 中文/转义地狱**(`\U`、`\\n`、mklink 引号)→ 单引号 heredoc、PowerShell、避免在 bash 内联写 Windows 路径。

---

## 五、现状与下一步

**当前状态**:游戏初始化推进到 `menus.tjs`(MenuItem 类缺失,下一个补丁点)。补丁循环已验证高效(每轮约 3 分钟),按此迭代可抵达标题画面。

**待办(按优先级)**:
1. 继续补丁循环至标题(menus → 后续符号)
2. 真机版验证(插件目录真实可读,行为可能与模拟器不同)
3. Phase 6c:手柄映射按 ONScripter 表优化、字体兜底(romfs 内嵌 font.ttf)、存档独立目录、多游戏浏览器
4. Phase 5:FAudio 音频验证、性能基准
5. Phase 7:打包发布(图标/NAC P 信息/README)

**既定约定**:
- 游戏目录:`sdmc:/switch/krkrsdl2/game/`(xp3 或解包目录)
- 补丁目录:`sdmc:/switch/krkrsdl2/patch/system/`(同名覆盖)
- 日志:`sdmc:/krkrsdl2_debug.log`
- 交付产物:`build-switch/krkrsdl2.nro`(唯一命名)
---

## Phase 3 Stage 3.2 破案记录 (2026-09-07 晚, v1.6→v1.8)

### run12 (v1.6) 破案:窗口切换后 glc 全灭
- 现象:readback 计数爬到 136,但 m5/snapshot 从不出现;用户反馈"只能进软件,进不了游戏"。
- 根因链:日志 `SDL_CreateWindow` **两次**(launcher 窗口+游戏窗口)。glc context 建在 launcher 窗口(`gWindow` 首次 BeginContext 缓存),launcher 窗口销毁后 `SDL_GL_MakeCurrent(死窗口)` 每次失败 → readback 136 次全部死在 BeginContext()(m5 日志在它之后,永不打印)。游戏窗口上从未有一次成功的 GL 合成;try_blt 在无有效 context 下 GL no-op 却返回 true → 引擎跳过 CPU blit → 黑帧。
- v1.7 修复:BeginContext 每次解析当前 renderer 窗口,`win != gWindow` 时 ResetGLState 全重建(context/FBO/纹理/探针/里程碑,新窗口重走 m1-m5);加 gFrameActive 帧活性门(try_blt 在 begin_frame 未完成时一律 false → CPU 兜底);readback 失败打 SDL_GetError。

### v1.7 真机验证(用户反馈"软件能打开,进游戏后只有一点点画面,其余黑屏")
- 日志确认修复生效:`window switched ... rebuilding GL state` → 第二次 context ready/probe PASS → **m4 first blt** → readback #1-#3 → **`frame snapshot saved (readback #1)`**(快照首次落地!)。窗口切换问题彻底解决,合成真正在游戏窗口跑起来了。
- 新问题"只有一点点画面":fallback rect **无条件记录**(GPU 成功路径也记)→ readback fold 用 compose bitmap 旧内容覆盖 GPU 新内容;且 **64 上限溢出**(每帧 ~250 blts,~90 GPU + ~160 fallback),超出 64 的 fallback 区域无折叠 → glClear 的透明黑。症状=部分 GPU 内容被旧像素盖掉 + 大片纯黑。
- v1.8 修复:NoteComposeFallback 只在**确定走 CPU** 的路径调用(hda/非 copy-alpha-add/光栅不可用/上传失败),GPU 成功路径绝不记录;上限 64→2048;fallback>128 时整幅折叠代替逐 rect 逐行;m5 直接打日志(Milestone 的全 true 守卫会吞掉最后一个里程碑,这是 m5 从不打印的原因)。
- 遗留观察:shader 输出固定 `rgb*alpha`(copy 模式未 disable),alpha<255 的图 copy 会变暗;等画面正确后按需修。

---

## Phase 3 Stage 3.2 破案续 (2026-09-07 深夜, v1.7→v2.4)

### 里程碑：真机画面第一次完全正常（用户确认"游戏正常了", run19）
- **状态**：mode 4（`gpu-composite-cpuonly.txt`）下游戏画面、按钮、点击全部正常。
- **技术真相（诚实记录）**：run19 日志 `mode4-direct: raster unavailable` 贯穿全程 → v2.4 直拷分支因 `krkrsdl2_glc_get_bitmap_raster(gComposeBitmap)` 返回 null **从未覆盖 surface** → 屏幕 = SDLBitmapCompletion 的 CPU 直拷画面。**画面正常 = glc readback 悬空不干扰，而非直拷成功**。这与 blt# 诊断 `destIsCompose=1`（gComposeBitmap 非空）并存——即 try_blt 时刻 gComposeBitmap 有效、readback 时刻解析光栅失败，是下轮要查的 gComposeBitmap/光栅访问器之谜。
- **核心结论（解释 v1.7 以来所有黑屏）**：
  1. surface 原始内容（SDLBitmapCompletion 直拷 CPU 合成）**全程正确**（视频闪过、按钮单独渲染、run19 完整画面均为证）；
  2. glc readback 每次用 fold+ReadPixels 或直拷结果**覆盖 surface**——覆盖内容几乎全黑（fold 链坏）；
  3. 任何 "glc 覆盖失败/不覆盖"的偶然路径（v2.4 直拷 null）都会露出正确画面。
- **待办**：① 查 gComposeBitmap 在 readback 时解析光栅失败原因（get_bitmap_raster 返回 null/异常）；② 修好 fold/ReadPixels 链后 mode 1（GPU 合成）才有性能意义;③ 期间游戏可玩（CPU 直拷=引擎原声性能）。

## v2.7 — "画面正常但更卡"定案 + mode5 单次合成 (2026-09-07)
- **v2.6e 真机反馈**：画面完全正常（条带拼合 ✓），但"比之前更卡"。
- **定案**：mode1 是**双重合成**——CPU 合成（90 次条带 blt，simde 已激活）照跑，GPU 又完整重画一遍（90 次条带上传 + 90 quad），再加全屏 readback + 逐像素 R/B 交换 + SDL 全幅纹理上传。每帧 GPU 链多 ~15-25ms（llvmpipe 侧），帧率掉一半是必然。
- **v2.7（mode5, gpu-composite-gpuonly.txt）**：跳掉 CPU 侧条带合成（BasicDrawDevice gate），GPU quad = 唯一画面源；readback gate 放宽到 ≥1 层；swap 改 uint32 旋转；readback 计时。**A/B 判据**：画面仍正常 → CPU 合成立即可整体移除 → 呈现链直连 FBO 纹理（下一大步）；画面错/黑 → bits 依赖 CPU 合成 → 必须 Kirikiroid2 式重写。
- 构建 ef54050e，P37。

## v2.8 — mode5 呈现直连 FBO（readback+全幅上传 移除）(2026-09-08, 13f0be4a)
- **真机确认（用户："游戏画面正常"）**：mode5（CPU 合成完全跳过）画面正确 → CPU 合成整体移除成立。
- **v2.8**：mode5 呈现 = glBlitFramebuffer(FBO→窗口 backbuffer) + SDL_RenderPresent（空队列只 swap）。砍掉 ReadPixels 3.7MB + swap + 全幅 UpdateTexture（3.29ms）+ RenderClear/RenderCopy。blit 带 LINEAR 缩放适配 dock 1920×1080。
- 剩余链：条带上传（90×1280×8）+ 90 quad 合成 + 1 blit + swap —— 全在 llvmpipe 批量三角形路径（E-mote GL 已验证高效）。
- 判据：真机帧率提升（对白 ≥60 / E-mote ≥30 目标）；若撕裂/黑帧 → gate 兜底自动回 SDL 链，看日志 `rb=` 与 `-> PUBLISH` 行。

## v2.8b — 黑闪根修 + 统计 ReadPixels 移除（7739d41d）
- v2.8 日志：fps=10.7 compose=102ms（动画场景引擎 CPU 合成）+ 周期 keep CPU 黑帧（gate 失败→空 surface 黑屏）+ rb13-16ms 统计开销。
- v2.8b：mode5 直接 blit（无统计、gate=layers>=1），黑闪消除；layersMs 计时就位。
- 下一步：compose 102ms 定位（对白 vs E-mote）→ Stage 3.2 引擎合成 GPU 化或 E-mote 图层直通。

## v2.8g（0920a252）黑闪根治 + 尺寸实证
- mode5 gate 移除：readback 必呈现（保留帧重显示），SDL 链黑 surface 帧不再存在。
- 呈现尺寸改用 SDL_GetRendererOutputSize；5 点探针（前 3 帧）定案尺寸/位置。
- 待真机：probe 矩阵 → 若内容只在部分探点 → 尺寸未对齐；视频帧层数据 → 视频路径判定。

## v2.8h 收官：GPU 合成呈现链闭环（b8ebc8f1）
- 窗口大小正常（viewport 修复）；RGB 正常（swapRGB）；黑闪消除（保留帧呈现）；启动页可见（SDL_GL_SwapWindow）。
- 视频黑屏 = null player（VideoOvlImpl Play 占位），Phase 4（FFmpeg 移植）待办。
- 下一步：①compose 102ms→Stage 3.2（引擎合成 GPU 化/E-mote 直通）②Phase 4 视频（WA2-ns 管线参考）。

## Phase 4 v1：FFmpeg 播放器落地（e2caf44b）
- SwitchMovieOverlay：WA2-ns 管线移植（解码线程/AVIO/节奏/sws/事件），iTVPVideoOverlay 完整实现。
- 链接 WA2-ns 静态 FFmpeg 7.1（out/ffmpeg_switch）。构建链闭合。
- 待真机：OP 视频画面/帧率/完成事件；音频 v2。

## Phase 4 v1.1：视频帧真正接回 KRKR layer（da68132a）
- 修复 Switch `WM_GRAPHNOTIFY` 主线程事件桥；此前 FFmpeg 可解码但 `EC_UPDATE`/`AssignMainImage` 被 Win32 条件排除。
- 修复双缓冲槽位不一致、Bitmap 未初始化、detached 解码线程与 FFmpeg 释放竞态；补齐 EOF flush、暂停墙钟和 AVIO 生命周期。
- 本地完整构建成功；真机请同时核对 `[movie] published` 与 `[video] applied`，音频仍待 v2。

## Phase 4 v1.2：XP3 内视频打开失败修复（48e01d47）
- 用户日志的顺序是 KRKR `TVPCreateStream("opmovie.wmv")` 成功（25,177,361 字节），随后播放器 `fopen` 报 ENOENT；因此视频尚未进入 FFmpeg，黑屏来自 `movie.tjs open` 的 `Invalid video size` 异常。
- `VideoOvlImpl` 现在把已解析的 `tTJSBinaryStream` 直接转交播放器，FFmpeg AVIO 在该流上 read/seek；删除本地路径猜测，统一支持 XP3/7z/autopath/松散文件。
- 同步修正 AVIO 缓冲分配、EOF、seek 校验和存储流释放顺序。完整 Switch 构建成功；唯一 NRO 为 `build-switch/krkrsdl2.nro`，MD5 `48e01d47ed27a27ed89bd4969dc7df6f`。真机下一步看 `[movie] storage ready` → `[movie] ready` → `[movie] published` → `[video] applied` 链是否依次出现。
