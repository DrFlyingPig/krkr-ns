# 性能与运行故障记录

更新：2026-10-01。本文合并剧情冻结/影片音频、退出等待、菜单启动回归、游玩性能和快速读档记录。保留故障原因、源码关系、日志与构建身份；同日不同候选的检查与用户反馈分别记录。当前兼容范围见 [兼容计划](KIRIKIROID2_PORTING_PLAN.md)，模块契约见 [模块说明](MODULES.md)，补丁实现见 [PATCHES.md](PATCHES.md)。

本地测量来自 Ryujinx/Nextendo；主机微基准、模拟器专项、实际游戏反馈与物理 Switch 验收不能相互替代。删除审批、个人路径和部署过程保留在未入库的本地开发记录。

<a id="current-build"></a>

## 当前包与验收状态

| 包 | 身份 | 运行证据与范围 |
|---|---|---|
| 当前正式包，2026-10-01 12:50:24 | 28,846,865 bytes；SHA-256 `AAEDB680E929DCFE16D6C9C9B8CCDD357E41C10E8F4A9A752972E927AD42629A` | build-switch/out/游戏列表入口三处一致；GUI `Ryujinx_1.8.11_2026-10-01_12-57-10.log` 加载 out，运行日志 `krkrsdl2_debug_1790830646.log` 确认 `Oct 1 2026 12:50:24`。用户已复测并确认快速读档、人物语音、菜单和剧情正常 |
| 前一修复包，2026-10-01 11:51:11 | 28,846,865 bytes；SHA-256 `D70119452ADA500ECBA6032DB6F0533929B82367FF1C0BB7C2ED772D3EF603F8` | 用户已确认人物语音与同一标题快速读档正常，作为已验收回滚包保留；不能把该反馈当作最新包手测 |
| 语音/X-map 包，2026-10-01 11:35:46 | 28,846,865 bytes；SHA-256 `B00CEBDB95ECDD4F271636ED5173169F860E6261FC9C1A0D6A91CEFF3C671864` | 用户先确认游玩/语音“似乎都正常了”，随后快速读档原生崩溃；已经被 Owner 修复包替换，不列为完整验收通过 |

最新包补线程池扩容失败后的串行/已有线程回退、自定义 AVIO buffer 释放和视频定位中 Switch 专用成员的宏边界。主机 15/15 通过，含实际 GL、窗口清理与五种构造阶段检查。线程失败注入比较 51 份完整输出、202 次任务 exactly-once，共 2,585,496 字节，mutex/condition/thread 无残留；旧 HEAD 负对照失败，新实现通过。视频两处 translation unit 使用真实 Switch 参数编译并检查平台宏。语音与 E-mote 源文件仍为下文锁定哈希，复用其最终 PCM/RGBA 证据。用户已完成本包实际操作复测；该结论不扩展为全部游戏、物理 Switch 专项或固定 FPS 改善。

<a id="exit-audio"></a>

## 2026-09-27：Riddle Joker 标题卡顿与退出黑屏

实际连续会话先进入另一游戏，再进入 Riddle Joker。日志 `krkrsdl2_debug_1790482374.log` 返回标题后平均每次更新约 337 ms，E-mote draw 约 311 ms，upload 约 1.92 ms，present 约 0.53 ms；内存和事件数量稳定。CPU 标记早已存在，初次标题也是软件后端，没有返回标题才新发生 GPU 失效的证据。

退出时日志执行 `kag.close()` 并播放 goodbye 语音，淡黑已完成；游戏 `sysscn/end.ks:40` 的 `[ws]` 没有解除，43 行 `terminator.invoke()` 和 50 行关窗未执行。原因分为音频会话、短声音 EOS、线程唤醒和有限动画状态，不能全部归为绘制卡顿。

| 原因 | 通用修复 | 回归范围 |
|---|---|---|
| 进程级 audio dispatcher/buffer 列表引用旧 TimerThread，重建后第二个游戏缺自然 stop 通知 | 重启释放钩子销毁并清空 dispatcher，新会话重新注册；旧对象迟延/重复 Invalidate 不影响新定时器 | `sound_timer_restart_test` 编译实际源码，旧版第二会话失败；覆盖 EOS 与旧对象清理 |
| `SamplesPlayed==0` 被直接当作 EOS，初次消费前的短语音也为零 | 只在 PCM 队列也耗尽时采用零计数 EOS，保留正常样本终点判断 | `sound_player_eos_test` 覆盖启动、正常终点、EOS 清零和重播；旧版零计数仍有 PCM 时失败 |
| Start 只发自动复位事件，未清 SuspendThread；定时等待消费信号后仍再次无限等待 | 先清休眠标志，发布 `ThreadCallbackEnabled=true` 后再唤醒；诊断改用线程安全的 Switch 日志入口 | `sound_event_wake_test` 确定性复现丢唤醒、已有无限等待及多次休眠/播放，旧版负对照失败 |
| 有限 E-mote 动画结束只清 playing，allplaying 留真，游戏继续更新已结束标题 | 扫描 motion 引用图，排除循环/参数化/引用环/不明引用；在自然结束、无变量驱动/活动 timeline 时清 allplaying，保留最终姿态更新；play 清 _isStop | `emote_playback_completion_test` 覆盖 19 项边界，不代表全部 timeline 能力 |

透明网格/像素跳过和全不透明 NEON 复制保留特殊混色、模板阈值、UV、几何与尺寸；scalar/NEON 各通过 123,303 项像素契约。

中间候选不能混作最终成功：一次音频包退出成功；12:57 包的有限动画结束使返回标题流畅，用户确认，但 goodbye 完整后仍黑屏，需要点击跳过 `[ws ... canskip]`。黑屏期间主循环 `main=ok`，缺 native EOS 通知；点击后可正常重建，不支持永久线程死锁。随后追加丢唤醒修复。

最终日志 `krkrsdl2_debug_1790486390.log`：音频 fixture 完成 56 项并返回生产启动器，进入真实游戏；2735/4537 行确认初次/返回标题自然结束。4786 行播放 `nan_goodbye.ogg`，4807 行执行 terminator，4809 行记录 EOS，4875–4876 行关闭并返回启动器。最后点击是退出确认，没有依赖点击跳过声音；跨线程日志相邻行顺序不当作精确时间线。用户确认标题与自然退出正常，`riddle-wake-fixed-exit.png` 显示启动器。

最终 host 11/11。历史正式包构建 `Sep 27 2026 13:17:33`，28,738,545 bytes，SHA-256 `5EF90E4E0DD15706E2552653A209FE79A102B3C13950A3BB49B1ADCD4665FD4A`。证据目录当时为 `build-exit-investigation/`，含最终 wake 日志、早期黑屏失败与连续 launcher 音频检查。结论限本地模拟器实际游戏路径。

<a id="raster-movie"></a>

## 2026-09-27：剧情冻结、Raster 与影片音频补给

《魔女的夜宴》实际剧情报 `layerExRaster plugin not loaded` 后停住。通用修复补原生 Layer.copyRaster 和可用性登记，不能继续将 Raster 列为当前缺失。Window finalize 中再次访问已失效的 extractTrigger 还可能重入错误清理：在窗口脚本 finalize 边界局部处理异常后继续原生清理，已取消登记不重复销毁，子对象失败不阻断窗体结束，无进展清理有界；通用 TJS finalize 和剧情异常规则保留。

实际影片 `羽.wmv` 内容为 MP4/H.264，1280×720，AAC 44.1kHz stereo，约 50.085 s。容器连续约 15 个视频帧（500 ms）再交付约 21 个 AAC 包（488 ms）；四个 16 KiB 音频槽最多提交三个，覆盖约 279 ms。旧视频等待不补提交已解出的 PCM，因而即使队列还有数据，设备仍可饿死。

修复在 video wait 中继续提交可用 PCM；非 EOF 的队列空不当作结束，设备槽满时保留未提交尾部；demux EOF 后继续排空 codec 和重采样。新播放重建源并清理旧同步/回调 credit，pause/resume 保持源和位置，不靠增大计数假装同步。

当轮 host 14/14，含 raster、movie_audio_queue、window_cleanup。Raster 模拟器夹具日志 77–192 行覆盖 17 项原生契约及负 pitch/边界；图片证据只显示夹具输出，不替代整段剧情。影片 fixture 初版错误轮询 status，后来以 onStatusChanged 纠正，不能把早期失败包当作通过。

纠正后的影片日志：476 行 demux EOF，477 行 decoder EOF、重采样尾帧 0/error 0，479 行发布第 1500 帧；484 行为 PCM 队列空、submitted/completed 均 540、8,835,072 bytes、buffered-starve 0，后续关闭音频/codec/format/AVIO/storage，504 行 `COMPLETE naturalEOFpolls=204`。PCM 时长 `8835072/4/44100=50.085442s`；计数/截图不能独立证明试听或同步。

用户随后确认实际剧情“现在正常了”，真实日志确认 Raster 加载，无反复脚本清理循环；后来明确“视频音频也正常了”，构成该游戏模拟器试听的验收。历史正式包构建 `Sep 27 2026 15:05:23`，28,742,641 bytes，SHA-256 `4497CCE555F97CD67EE48BA2221A32C62693E57E3E8CE2DBE4BE86ABA403E907`。独立 fixture 替换 RomFS，其整体 NRO 哈希不应混作正式包身份；物理 Switch 专项仍未验收。

<a id="menu-startup"></a>

## 2026-09-30：菜单移植改变能力探测，导致 LimeLight 启动失败

原 v0.1.0 基线提交 `4027d2f1b7dce66d833eb5d5619ef4272340ead5`；独立 Release NRO 为上一节 `4497CCE...`。旧 k2compat 在 if 中声明的公共 `class MenuItem` 会被 TJS 提升，实际对游戏可见。移植改成私有类，仅在 no_game_layer 路径导出，LimeLight 因而换走 DummyMenuItem 分支。

先修每窗口私有根仍不足：公共类继续缺失，`DummyMenuItem.visible` 调用 `parent_.updateList` 时进入不实现该协议的兼容根而异常。日志 `1790733730` 先缺 Window.menu；`1790734612` / `1790734759` 首个故障为 Dummy updateList。失败发生在任何 popup 输入之前，游戏没有显式 link menu.dll，不能归因于渲染后端或 native 插件被强行加载。

修复恢复 Release 的公共 MenuItem 可见性，仅在名字未定义时导出，保留游戏/native 类优先级和每窗口独立根；没有给错误类型加空 updateList 以吞异常。独立的补丁包装问题也修正：Storage.deleteFile/copyFile 等有副作用的调用不再置于 `if(result)` 中，丢弃返回值仍执行动作。

旧 Release 在同目录/存档可进标题；恢复能力后当前生产包进入标题、设置、剧情，用户确认“现在在运行的这个能正常进入游戏游玩”。9 月 30 日 10:31:41 包为 28,810,001 bytes，SHA-256 `25367219B426A89C7EDCDF344C8EB29200CE00DEE65E5A80B448D143FB775073`。

此前树/root/index fixture 没有覆盖能力分支与 visible/updateList 协议，遗漏了启动回归。新增 `menu_game_layer` 夹具含派生构造、字符串快捷键、visible/onClick，但最终生产状态未单独重跑该新增夹具；不把脚本 36 项/native 20 项树检查扩大为它已通过。后续 Window 修复见 [快速读档](#quickload-owner)。

<a id="performance"></a>

## 2026-09-30 至 10-01：持续低帧的归因与优化

### 真实绘制模式和统计口径

初次日志 `krkrsdl2_debug_1790735577.log` 显示 CPU composite mode=0、full-frame upload、E-mote CPU fallback。`emote-cpu.txt` 是 9 月 15 日已保留的配置：当时模拟器软件 GL 加回读更慢，不是本次可直接删除的错误标记。后端已在进程选定，改标记不会自动切换；GL 仍须通过 FBO/回读自检。

此路径同时执行 CPU E-mote 三角形、CPU Layer 合成和最终整幅 SDL 上传，1080p 每次约 7.9 MiB。两边已有 SIMD/行带并行，不能称为完全没向量化；`rb` 指标在 CPU 模式主要是像素转换/比较，不自动表示 GPU 回读。

| 动画窗口 | 更新频率 | 墙钟/更新 | compose | E-mote draw | progress | upload |
|---|---:|---:|---:|---:|---:|---:|
| 当前初次日志 4591–4593 | 8.2/s | 122.7 ms | 59.44 ms | 42.17 ms | 4.44 ms | 2.13 ms |
| 同会话 10755–10757 | 12.6/s | 79.6 ms | 50.18 ms | 33.10 ms | 3.91 ms | 2.10 ms |
| 旧 Release 对照 2967–2968 | 12.5/s | 80.0 ms | 57.78 ms | 40.70 ms | 4.55 ms | 2.12 ms |

这些不是严格对齐回放。compose 包含 onPaint/E-mote，draw 包含像素转换，dispatch 又包含脚本/窗口绘制；嵌套指标不能相加。fps 是 updates/墙钟，含用户等待和加载；ev/disp/tick/wait 按 loops，绘制项按 updates，比较前需乘 loops/updates。静态画面 compose 约 0.5 ms 且 E-mote=0 时，10 次更新/秒不表示只能绘制 10 FPS。

冷加载另有约 12 万文件的 autopath 重建（约 290 ms）、图片解码 228–298 ms 和 dispatch 1117.6 ms 尖峰。旧记录部分游戏请求约 10 Hz，实际请求率、完成率、等待和绘制成本要分别判断；本轮不改游戏时钟。最终上传约 2 ms不是上述动画场景主因，普通事件/菜单桥也没有主导该窗口的证据。

### Kirikiroid2 的参考关系

固定参考 `d1c2b125...` 的 [RenderManager.cpp](https://github.com/zeas2/Kirikiroid2/blob/d1c2b1259423542c893e0b65eaeb46c848848f2b/src/core/visual/RenderManager.cpp) 默认软件模式；1004 行等软件像素操作使用 factor×500 面积门槛，3133 行起识别矩形，3292 行批量提交三角形，1386 行自引用复制单线程。[LayerIntf.cpp](https://github.com/zeas2/Kirikiroid2/blob/d1c2b1259423542c893e0b65eaeb46c848848f2b/src/core/visual/LayerIntf.cpp) 6802 行、[RenderManager_ogl.cpp](https://github.com/zeas2/Kirikiroid2/blob/d1c2b1259423542c893e0b65eaeb46c848848f2b/src/core/visual/ogl/RenderManager_ogl.cpp) 3631 行与 [MainScene.cpp](https://github.com/zeas2/Kirikiroid2/blob/d1c2b1259423542c893e0b65eaeb46c848848f2b/src/core/environ/cocos2d/MainScene.cpp) 904 行体现可选完整 GPU Layer 链。

本轮将这些通用策略适配至 CPU 后端，不宣称复制 K2 未公开的 E-mote 插件。重叠三角形依然按索引顺序混合，线程只拥有独立目标行；几何/UV 不用近似判定。完整 GPU 管线仍需纹理、混合、裁剪、蒙版和更新语义，不是启用 gpuonly 或最终 GL quad 的同义词。

### 保留与撤回的优化

| 改动 | 严格范围 | 关键验证/局部测量 |
|---|---|---|
| P94 NEON texel 坐标 | 无蒙版、扫描宽度≥64，保留原 FMADD/FMLA 融合舍入与边界夹取；蒙版/小网格旧路径 | 普通/FMA 配置各 11,834 场景、23,668 完整缓冲对照；主机无蒙版全屏 DrawMesh 中位耗时少约 4.96%，非游戏整帧收益 |
| P94 AutoPath 1024→16384 桶 | 原哈希、覆盖顺序、追加/移除/compact/reset，不保留过期资源 | 合成 48,001 名称、约 120,006 条记录，两版各 3485 检查；旧/新/旧重建中位 379/239/378 ms，约少 37%，仅合成资源场景；静态表多 1,105,920 bytes，不等于运行时总涨幅 |
| P95 网格有序行带/矩形/双 UV | 大网格、严格互补共享边/外边覆盖；保留两套 UV 浮点递推和逐像素三角形选择；自引用/旋转/不确定回退 | 6000 场景×3=18,000 完整缓冲、1,167,379,008 bytes，另 36 别名检查；主机大矩形约少 44%、4×4 网格约少 27–30%，不套用到实际未命中的网格 |
| P96 可用核心 | `svcGetInfo(InfoType_CoreMask)` 数进程授权核心；失败使用至少1个 SDL 核；不改优先级/亲和性 | 19:57 真日志 drawT=3/mask=7，两个 worker 和调用线程在核心2/1/0重叠执行，17.56/19.40/18.08 ms、总19.46 ms；排除全串在单核 |
| P95 源行复用/连续读取 | 无蒙版普通混合、V对X严格零；原 U 递推/尾部/颜色/GL_MAX-alpha；源行不等回退，别名排除 | 加 720 大矩形更新、192 长扫描线/细三角形，三舍入配置对照；源码与实际人物 rowSample 日志命中，动作不同不量化前后收益 |
| P97 回拷不变组/边界提取 | 仅写变化组，首末变化组提精确脏区，保留透明擦除/负 pitch/尾部 | 生产函数与独立逐通道参考比较2400组、199,826,952 bytes；主机1920×1080未变/局部少17.28%/15.08%，全变多2.59%，如实保留 |
| P98 内置 crossfade/universal 并行 | 沿 K2 59×500/46×500 门槛，每线程≥32行；调用线程先取位图/COW，实际内存重叠串行 | C/SIMD共3600完整缓冲、1,811,712,980 bytes，1288重叠输入/684真实并行批次；主机局部少约50–59%，实际转场命中但不是持续低帧主因 |
| P99 X-map | 普通混合、无蒙版/别名，U不随Y/V不随X；原融合取整/四像素递推，多表有预算，行Y不同回退 | 最终普通NEON/FMA/边与UV融合三配置各19,164缓冲，CPU/NEON契约123,303；覆盖31/32/33表和16,384索引边界。满opacity主机三场景少7.62%/7.23%/3.63%，不代表整体FPS |

扩大全部 UV 向量化曾使带蒙版/小网格回退，已限范围；小网格矩形识别回退后将门槛设为至少33,000包围盒像素。扫描空组试验只获约1–2%局部改善，另一共享边合并在子像素矩形多8–19%，均撤回。full-opacity 整数混合穷举16,777,216通道组合与像素对照虽一致，主机 SSE2 多42–56%、AVX2多104–114%；A57静态指令数少不证明运行更快，已撤回，正式仍原浮点混合。

最终 E-mote 源 SHA-256 `CD9B2360D0FFC46E9DA5569C4D5E871730DEFCA4FF53A25B08A7C76746922D8D`。最终每配置19,164完整缓冲包括18,000基础、108次opacity0.73/1/2别名、720大矩形、192长扫描线和144 X-map专项。源文件和主机输出证据不代替真机浮点、其他游戏流程或当前用户画面。

历史 GPU/画质失败教训继续约束本轮：mode1 重复 CPU/GPU/回读；绑定旧窗口、错误回退区域、增量 FBO 清空导致缺画；P53 自动大纹理上传/SDL_RenderReadPixels 真机崩溃不能概括成 diffrows 必然错误；quarter 的读回25%不能推断FBO只画四分之一；用户已否决半分辨率。本轮保持原尺寸、后端标记和游戏节奏，旧方案未重跑。

### 实际复测：局部路径命中与体感分开

14:30 GUI 实际仍加载旧 build-switch 包，因此那次运行不是候选证据；14:34 GUI 明确加载优化路径，并见 vector/FMA 标识，才确认身份。相近标题窗口 draw 42.39→40.03 ms、compose 62.87→57.78 ms，更新10.9→12.3/s，但等待少7.4 ms/update、合成量7.48→6.46 M/update，不能把整个变化归因UV。局部量级约2 ms，不足使十余次/秒明显流畅；图片解码326.77 ms与资源重建286 ms仍在。

| 已核身份的历史阶段 | 实际日志/行为 | 用户反馈与限制 |
|---|---|---|
| 15:30 资源索引包 | `1790765050`；compose57.30/draw38.48 ms，标题11.9更新/s，剧情compose27–28 ms | 所有场景无明显改善 |
| 19:17 行带 | `1790767224`；ordered mesh bands命中，draw25.52 ms，但网格/动作不同 | 无明显改善，不作严格前后比例 |
| 19:37 严格矩形 | `1790768459`；有批处理，无exact rectangle标识；draw39.85 ms/11.7更新/s | 快路径没覆盖低帧片段，不能套主机矩形收益 |
| 19:57 三核 | `1790769542`；三核并行已核；draw40.54 ms/12.4更新/s | 仍无明显改善，任务数量不是主要解答 |
| 20:12 双UV | `1790770518`；全屏背景splitUV实际命中21.54–24.08 ms，draw34.18 ms | 仅有限下降，未验收整体改善 |
| 20:43 行复制 | `1790772841`；背景rowCopy约9.48–11.04 ms，标题draw42.48 ms/11.8更新/s | 仍卡；模拟器已1.8.10→1.8.11，数值不能全归NRO |
| 22:00 源行 | `1790776964`；人物rowSample命中7.88 ms；标题prepare/raster分别16.36/12.43及35.95/7.87 ms | prepare含E-mote；设置切换raster21–22 ms，加载317–648 ms，无稳定普通剧情基准 |
| 22:20 回拷 | `1790778309`；剧情两窗口25.3/25.9 ms，raster15.91/7.97 ms | 用户确认有小幅改善，提升不大；不是全部场景稳定39FPS |
| 22:56 内置转场 | `1790780380`；crossfade约0.3–0.4 ms，universal约0.6 ms已并行；E-mote每更新7–9网格draw39–44 ms | 用户仍称帧率低；更新74–89 ms/11–13次，prepare含E-mote不能叠加 |

各阶段包身份用于复核实际加载，均为历史包：

| 构建/候选 | SHA-256 |
|---|---|
| 14:34 UV 独立候选 | `603572A5F4795AB7903FD811D28286F02DCABDF0607A8C46C8F80430988B47B6` |
| Sep 30 15:30:55 | `5AC1DD10DCAE2C84D8E391012AE775BD1C81C9F95439CECD1B59025EF8ED2D77` |
| Sep 30 19:17:42 | `273C146E77E2E07D23952EADDB58B60E1D7CC2BA83FBD450E5B2202E7FB923F9` |
| Sep 30 19:37:35 | `519ACF786C78F3954327C7958A3FE40093A5B3DA4ADBE8F8D082D06184803BBF` |
| Sep 30 19:57:13 | `FA8B2721BF7D8FEB250C775758579CA372E038AFDBA11ED90A488A1312F25946` |
| Sep 30 20:12:00 | `222CFC3A9C4D6791A18344320BE79D9E69ED146F6DA10B38BD4B78E0CDAB9BC8` |
| Sep 30 20:43:47 | `5A370308E9D2B99F02B47CE08EDA5168593F340A8D6E7FC510FCB047C010A852` |
| Sep 30 22:00:32 | `4A4C0A7B18B1F3EA09231C06F52A1F9E3E2BB511AF69F80FF0BA170A03D80167` |
| Sep 30 22:20:40 | `EA0660D20330A426F8244CCFEE7C10E1C1776B809FFF34551715DD2F7A056A78` |
| Sep 30 22:56:25 | `0E96842DB44B3F482835E91FC900DEC82E57D3E796C02AD769E61C43EE9657A1` |

以上散列应与原记录/文件一起使用，构建时间不能单独识别只重编单个源文件的候选。9 月30性能证据当时保存在 `build-game-perf-20260930/`（完整像素、FMA、配对benchmark、对象/反汇编、runtime-comparison），以及后续本地源行/X-map对照日志。继续优化需对齐实际卡顿操作；当前仍未得到整体固定提速或物理 Switch 结论。

<a id="aac-voice"></a>

## 2026-10-01：人物语音与旧启动入口

11:07 GUI 加载 `portable/games/krkrsdl2.nro`，日志 `krkrsdl2_debug_1790824057.log` 的标识仍是 `Sep 30 2026 10:31:41`，哈希 `25367219...`。此前只从 out 路径启动而未同步游戏列表入口，不能把本次运行当作22:56包效果。后续同时同步入口并核GUI加载路径/哈希/标识。

《千恋万花》的人物 `uts001_001/002`、`rok001_006/007/008` 和系统语音 WaveSoundBuffer.open 抛错，BGM/SE仍内置Opus。只读提取三份失败文件虽名 `.ogg`，内容为 MP4/M4A AAC，48kHz mono。WAV/Vorbis/Opus不识别，参照K2 FFWaveDecoder增加通用内容探测；不改文件名、资源或跳过异常。

最终 `FFWaveDecoder.cpp` SHA-256 `509018AEE0060FFED9D99C3E5E41326171B56CF0550C112922D173F1D412B779`。直接编译生产解码器，与同版 FFmpeg CLI 比较2,749,056 PCM字节，完整输出均逐字节一致：

| 样本 | 采样率/声道 | 输出 sample granules |
|---|---|---:|
| `uts001_001` 人物语音 | 48kHz mono | 57,056 |
| `rok001_006` 人物语音 | 48kHz mono | 354,016 |
| `roka_senren` 系统语音 | 48kHz mono | 52,960 |
| 合成 AAC | 22.05kHz mono | 45,056 |
| 合成 AAC | 44.1kHz stereo | 89,088 |

五样本 seek0/1/512/1024与连续PCM切片严格一致；尾部剩100帧短读、重复EOF不重播、超大定位拒绝与20轮无效输入storage释放均检查。仅flush重播曾有AAC系统语音差异，改重建codec，保留初始负时间戳和两帧preroll；重建失败clean/null，Render/SetPosition安全拒绝。随机中途seek允许AAC预测/PNS/SBR状态局部数值不同，最小cosine0.991211、±128样本搜索最佳位移为0，不称为任意定位bitexact。

登记逆序查找保持 Opus → Vorbis → RIFF → FFmpeg；旧Opus/Vorbis源码未改。核对WaveLoopManager消费者的sample-granule/EOF/PCM/定位及AVIO寿命，真实Switch参数编译通过，静态库MOV/AAC定义与登记存在，无需重建依赖。

11:35正式包GUI `Ryujinx_1.8.11_2026-10-01_11-36-59.log` 加载out，`krkrsdl2_debug_1790825834.log`确认构建。真实《千恋万花》命中xMap及不等价回退；`rok001_009/010.ogg`和系统语音成功`[audio] built-in FFmpeg opened`，BGM/SE仍Opus，原语音open失败不再出现。用户确认“似乎都正常了”，构成这次游玩/试听证据；随后Quickload故障需另修，不能将该反馈扩为所有操作或真机。

设置/退出仍有已捕获的 `WaveSoundBuffer.StkFreeVerb`、`Scripts.saveDataPack` 及部分脚本owner缺成员记录，游戏可返回启动器不代表这些接口完整兼容。渲染/波形优化不能证明全部初始化、配置保存或清理语义已解决。

<a id="quickload-owner"></a>

## 2026-10-01：快速读档暴露未初始化 Window.Owner

《晴菜花》标题快速读档在11:35包崩溃，GUI `Ryujinx_1.8.11_2026-10-01_11-40-35.log`，引擎 `krkrsdl2_debug_1790826045.log`。native PC `0x0072006B002F0068` 为UTF-16 `h/kr`字节形状；对应正式ELF：`0x50d54`为SDLWindow Invalidate进入基类，LR `0x1d6750`为BaseWindow菜单close参数 `tTJSVariant window(Owner, Owner)`。从Owner读取虚表后首次AddRef跳入上述文本，后续在Timer触发的TJS Finalize链。该日志三次音频打开均Vorbis，没有FFWave对象，不能据此撤回AAC或X-map。

真实XP3脚本与存档副本只读核对：

- TitleObject961–972暂停DefaultTimer、DataLoadAction、恢复LoadAction；MainWindow2381从BMP尾读取压缩TJS字典并复制/失效临时字典，没有native Window构造。
- MainWindow2801的ScreenFormat先读frm_meswin/meswin_map.png，再在2725/2730/2735/2740清Log/Title/SaveLoad/Config；旧日志恰停在两图之后。
- BaseObject13继承Window，42–52构造仅保存MainWnd/PriLayer/DeleteObject，不调super.Window；派生辅助类只调super.BaseObject。_SOUNDOBJECT还多继承WaveSoundBuffer与BaseObject，不应为其强造窗口。主MainWindow在Init_fix600创建，不是Quickload重建主窗。
- 原savedata001.bmp SHA-256 `56CBDD10766D908C7F0619647C1C1278E068D2DE1BFC7AD5CB3B5D80B06A4D77`；BMP43,254 bytes之后为有效压缩TJS字典，未修改原存档。

TJS先创建/attach native instance，再由脚本named constructor调用native Construct。省略基类构造或提前抛异常时，实例已挂载但BaseWindow.Construct从未赋Owner，失效仍进入native Invalidate。C++构造遗漏Owner初始化；**此前菜单移植新增 `__krkrnsMenuClose` 参数的对象引用，首次解引用该垃圾指针，暴露潜在未初始化问题。**旧流程只将Owner用于事件键等不解引用操作，所以此前同一辅助类通常能结束。

最小通用修复是在C++构造赋 `Owner = NULL`，真正Construct后仍原规则绑定。未构造辅助实例没有真实窗体或菜单，不伪造owner、不改游戏super调用或存档。本轮菜单验收遗漏了“继承Window却不调基类构造”，已补回归。

`tests/window_construction_test.py`提取真实Base ctor/Construct/Invalidate与SDL ctor/Invalidate，以非零UTF-16路径字节填存储后placement new；测试native已挂载后的受控寿命，不声称覆盖整个VM。当前五情形：省略super、super前抛异常、正常菜单、super后抛异常、draw factory抛异常；检查菜单引用/关闭与窗口清理。旧流程因果对照：

| 实际构造/清理组合 | 结果 |
|---|---|
| Git HEAD `4027d2f1` 旧ctor+旧BaseInvalidate+当前SDLInvalidate，`--git-head-lifecycle` | 未Construct/pre-super异常均安全完成，旧流程不解引用菜单Owner |
| 旧ctor+当前菜单Invalidate，`--git-head-constructor` | 新variant Retain/AddRef处受控拒绝垃圾Owner，exit1，不是提前字段断言 |
| 修复ctor+当前菜单Invalidate | 五情形PASS，正常菜单行为继续执行 |

原窗口清理测试也通过普通退出、script finalize抛错、菜单根引用、子对象失败、无owner与无进展保护。构造测试SHA-256 `DA86EC77273F93AFE7AF80C76C4A15D6A0A1129900C963544D415C40C77066D3`，修复WindowIntf.cpp SHA-256 `3E23828F469606329304389DB2AC5B8A9519F11B51A82FC968E93C168F435D10`。

11:51修复包的GUI `Ryujinx_1.8.11_2026-10-01_11-55-16.log`加载out，日志 `krkrsdl2_debug_1790826927.log`标识 `Oct 1 2026 11:51:11`。用户同一标题Quickload确认“读档正常”；1157–1171行读同一存档后继续背景、BGM、@0000_B00017/18/19人物语音，随后游戏正常退出回启动器，存档hash未变，无旧guest崩溃。初始化onResize缺dm的既有异常仍被忽略，不扩为所有脚本问题已解决。最新12:50包随后也已获用户确认，见 [当前身份](#current-build)。
