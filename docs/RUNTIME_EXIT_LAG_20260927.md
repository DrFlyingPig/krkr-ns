# 2026-09-27：Riddle Joker 卡顿与退出黑屏

本次排查针对用户 12:12–12:16 的连续游戏会话。此前 MenuItem / VideoOverlay getter 修改仍保留；没有修改游戏脚本或游戏资源。

最终状态：13:17 构建的修复 NRO 已于 13:24 正式部署。用户确认返回标题恢复流畅；追加音频唤醒修复后，真实游戏退出无需点击黑屏即可自动回到初始游戏库。以下保留中间失败复测，避免把早期局部成功混作最终验收。

## 故障证据

- 用户日志：`krkrsdl2_debug_1790482374.log`。同一 NRO 首个晴菜花游戏在 2400–2505 行完整返回启动器，随后启动 Riddle Joker。
- 返回标题后，每帧约 337 ms（3 fps），E-mote 绘制约 311 ms，上传约 1.92 ms，present 约 0.53 ms。内存和事件数量稳定，没有发现 OOM 或事件风暴。
- 模拟器 `emote-cpu.txt` 是 9 月 15 日已有配置。初次标题也是 CPU 后端，返回标题并没有新发生 GPU 失效。最初动画停止后成本消失，返回标题后的背景持续绘制。
- 6132 行调用 `kag.close()`，6228 行播放 `nan_goodbye.ogg`，6244 行恢复绘制速度，淡黑已完成。游戏 `sysscn/end.ks:40` 的 `[ws]` 等待自然声音结束一直未解除，43 行的 `terminator.invoke()` 和 50 行的关闭窗口未执行。

## 通用引擎修复

1. 音频状态检查定时器与 buffer 列表是进程级静态对象。引擎重启销毁 TimerThread 后，它们仍可能引用旧会话，第二个游戏得不到自然 `stop/onStatusChanged`。在队列音频重启释放钩子中销毁并清空 dispatcher；新会话重新注册；旧对象迟延或重复 Invalidate 不影响新定时器。
2. `SoundPlayer::Update()` 原本把任何 `SamplesPlayed == 0` 视为 EOS。新 voice 首次消费前也是零，导致 300 ms 短 Opus/Vorbis 实际只播放约 70–105 ms。现在仅在队列也耗尽时使用零计数 EOS 兜底，保留正常样本终点判断。
3. E-mote CPU 路径跳过确实无效的透明网格和透明像素；NEON 全不透明像素组直接复制。保留 multiply-add 模式、模板阈值、UV、几何、采样与分辨率。运行日志每 240 个网格汇总 opacity 分布，以验证实际收益。
4. E-mote 有限动画自然结束原本只清 `playing`，`allplaying` 自 `play()` 后始终为 true。实际游戏 `AffineSourceMotion.isFlip` 用 `allplaying || _lastPlaying` 决定是否逐帧更新；标题动画已结束仍留在 `MainWindow.flipLayers`。现在播放时一次扫描实际 motion 引用图，排除循环、参数化、引用环及无法解析的引用，仅在既有自然结束分支、无变量驱动及无活动 timeline 时清 `allplaying`，保留最后姿态更新。`play()` 同时清 `_isStop`，支持同对象停止后重播。

## 中间复测与追加音频修复

- 首轮音频修复包连续两次音频会话通过 112 项严格检查。真实游戏一次播放 `may_goodbye.ogg` 后自动返回游戏库，保存为 `riddle-audio-fixed-exit-runtime.log` 与 `riddle-user-exit-current.png`。
- 12:57 候选包修正有限动画状态后，实际日志 `1790485138` 在 5152 行确认标题完成于 frame 180；之后一个 26.9 秒窗口只执行 4 次 progress/draw。用户确认返回标题已流畅。统计窗口含长时间静止等待，不能把平均更新频率当作动画帧率。
- 同一候选包退出时播放 `chi_goodbye.ogg`，语音完整结束，但仍黑屏等待。用户点击后才通过游戏 `[ws ... canskip]` 继续并返回游戏库；这不是自动退出验收通过。保留 `riddle-completion-still-black-runtime.log`、`riddle-completion-click-released-runtime.log` 与黑屏截图。
- 黑屏期间主循环一直 `main=ok`；音频 native EOS 日志在整个真实游戏阶段缺失。点击跳过后音频线程释放及引擎重建成功，没有永久线程死锁的运行证据。继续检查音频线程暂停/唤醒和完成通知，不修改游戏的等待逻辑。
- 音频工作线程的 EOS 诊断已改用 Switch 原生日志入口，避免访问明确非线程安全的 TJS 日志共享状态。
- 进一步发现播放线程 `Start()` 原本只设置自动复位事件，没有清除 `SuspendThread`。定时等待先消费该信号后，线程仍可能按残留标志再次无限等待。现在先清休眠标志再发信号，`StartPlay()` 最后先发布 `ThreadCallbackEnabled=true` 再唤醒。该变化不增加轮询或绕过游戏 `[ws]`。

## 回归方法

- `sound_timer_restart_test` 编译实际 dispatcher 源码；旧实现第二会话失败，修复后覆盖 EOS、旧对象迟延和重复销毁。
- `sound_player_eos_test` 编译实际 SoundPlayer 源码；旧实现在零计数仍有待播 PCM 时失败，修复后覆盖启动、正常终点、EOS 清零及重播。
- `sound_event_wake_test` 编译实际线程 `.cpp/.h`，确定性控制自动复位事件在定时等待中消耗唤醒。旧 `Start()` 同一测试失败于“worker entered an infinite wait after its timed wait consumed Start”；修复覆盖该交错、已进入无限等待时唤醒、三次休眠/播放循环。13:17 候选包完整 host 测试 11/11 通过。
- E-mote scalar 和 SIMDe NEON 使用相同像素检查，覆盖透明、半透明、不透明、负/零/超量 opacity、特殊混色、模板 127/128、裁剪及绕序；每条 CPU 路径 123303 项检查。
- `emote_playback_completion_test` 使用生产引用图与聚合判断，覆盖 19 项有限/循环/参数化/共享子节点/引用环/活动 timeline 边界；不代表完整 timeline 调度已实现。
- `tests/build_sound_eos_fixture.py --launcher-cycles` 构造独立测试 NRO，通过生产启动器连续进入两次音频测试。测试合成 Opus/Vorbis/PCM、重播、800 ms 主线程停顿；可仅在本地附加真实退出语音。时长从 `play` 通知计至 `stop` 通知，不把首次音频初始化时间算入。

本轮构建、原始日志、测试包、截图和部署核验保存在 `build-exit-investigation/`。模拟器运行和部署结果以该目录的最终日志及哈希记录为准；真机表现需要单独确认。

## 最终运行与正式部署

- 最终候选实际日志 `1790486390` 保存为 `build-exit-investigation/riddle-wake-fixed-runtime.log`：482 行音频 fixture 完成 56 项检查，随后通过生产启动器重建进入 Riddle Joker。2735/4537 行分别确认初次标题与返回标题自然完成。
- 用户在同一轮游戏退出后反馈正常。4786 行播放 `nan_goodbye.ogg`，4807 行执行 `terminator.invoke()`，4809 行记录自然 EOS；4875–4876 行游戏关闭并请求返回启动器。最后一次鼠标点击是退出确认，之后没有点击跳过声音等待。跨线程日志写入的相邻行顺序不作为精确计时依据。
- `riddle-wake-fixed-exit.png` 显示已回到初始游戏库。此前黑屏靠点击才返回的候选不列为最终验收通过。
- 最终 host 测试 11/11 通过；旧实现的 dispatcher/EOS/丢唤醒分别有失败基线。运行证据范围是本地 Ryujinx 与上述真实游戏路径，不代表物理 Switch 或所有游戏已验收。
- 正式 NRO 构建时间 **2026-09-27 13:17:33**，部署核验时间 **13:24:53**，大小 **28,738,545 bytes**。`build-switch/krkrsdl2.nro`、`out/krkrsdl2.nro`、模拟器 `portable/games/krkrsdl2.nro` 的 SHA-256 均为：

  `5EF90E4E0DD15706E2552653A209FE79A102B3C13950A3BB49B1ADCD4665FD4A`

- 6 个 SD 兼容补丁与源码哈希匹配，已退休的 `motion.tjs` 没有残留。前一正式版本备份于 `.zcode/deployment-backups/20260927-132453/`。
- 已移除本轮专用 `000_SOUND_EOS_CONTRACT` 测试游戏/测试存档目录与两个专用进度标记；保留本地日志、测试包及用户存档备份，没有用测试前备份覆盖用户后续游玩进度。完整记录在 `build-exit-investigation/deployment-verification.json`。
