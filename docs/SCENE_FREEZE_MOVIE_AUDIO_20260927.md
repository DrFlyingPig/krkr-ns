# 2026-09-27：剧情卡死、退出清理循环与视频音频断流

本轮从《魔女的夜宴》实际游玩中“音乐继续、画面停在白色转场和 Save/Load 菜单、点击无响应”的反馈继续排查。剧情卡死的直接触发条件、退出异常的放大路径和视频音频供给问题分别记录，避免把它们合并成一个已验收的结论。

**最终状态：用户先回复“现在正常了”，随后确认“视频音频也正常了”，当前真实游戏剧情和视频音频均得到用户确认；14/14 host 回归、17 项 Raster 模拟器检查和真实影片素材的完整自然播放/关闭测试通过。15:05 构建的 NRO 已于 15:17 正式部署到 out 和模拟器游戏目录，当前运行保持同一候选包。** 物理 Switch 尚未验收；GitHub Release 未更新，源码改动尚未提交。

## 实际剧情卡死的证据

原始现场保存在 `build-scene-freeze/engine-current.log.gz`、`ryujinx-current.log` 和 `current-scene*.png`。现场 Ryujinx PID 为 26144，画面更新降到 0 FPS，而音乐继续。原快照经 gzip 压缩后核对解压 SHA-256 与原文件一致；下列行号均按解压后的日志内容计算。大型 console 日志另存为 `build-scene-freeze/krkr.console.log.gz`，同样已核验，未压缩重复文件退休。

`engine-current.log.gz` 解压内容的关键顺序为：

| 行号 | 事件 | 含义 |
| --- | --- | --- |
| 15065 | `layerExRaster plugin not loaded` | 本轮剧情/转场路径依赖的插件没有原生注册。 |
| 15068–15070 | 返回启动器请求、重建待处理、跳过系统最终 uninit | 游戏退出请求已收到，但尚未进入真正的引擎重建。 |
| 15071 | 再次提示缺少 Raster 插件 | 清理期间仍执行到游戏脚本的恢复/退出路径。 |
| 15077–15090 | `mainwindow.tjs(2136) finalize` 中 `extractTrigger.cancel()` 抛出 `The object is already invalidated`，随后调用 `System.inform` | 处理初始错误时发生第二个异常。 |
| 15091 起 | 再次请求返回启动器，然后再次运行同一 `finalize` | 异常处理和退出清理重复，主循环无法到达重建入口。 |

该日志快照共 1,052,398 行，包含 61,013 次 `mainwindow.tjs(2136)` 异常和 61,014 次 `terminate with engine restart pending`，没有 `[reinit] ===== restarting engine in-process =====`。这说明 CPU 忙于清理和异常处理循环；“音乐继续”不能证明主线程仍能处理输入或呈现画面。

## 通用修复：原生 Raster 插件和退出清理

`krkrsdl2/src/plugins/layerexraster/LayerExRaster.cpp`、`RasterCopy.h` 将参考 `krkrsdl3/plugins/LayerExRaster.cpp` 的 `Layer.copyRaster` 像素语义适配到现有 CPU Layer 缓冲区。CMake 加入注册单元；`src/core/base/sdl2/PluginImpl.cpp` 加入内置插件识别和静态归档链接锚点，经 ncbind 的实际模块注册表加载。它是可调用的原生实现，不是只让 `Plugins.link` 返回成功的占位。

实现保留逐行正弦位移、整数截断、时间相位和图像尺寸不匹配时不改目标的行为；未覆盖的边缘保留目标像素，复制包含原始 alpha，支持独立 pitch 和原地复制。先取得可写目标再读取源，遵守 Layer 写时复制。零 lines/cycle、非 Layer 输入和参数不足明确报错。参考接口不主动 `update()`，重绘仍由调用者负责。参考许可证随插件源码保存为 `LICENSE.krkrsdl3`。没有修改游戏脚本来跳过转场。

退出清理的问题在 `src/core/sdl2/SDLApplication.cpp`：

1. `process_events()` 检测到退出和重建请求后，先调用 `krkrsdl2_release_leftover_windows()`。
2. 原先对窗口 owner 调用 `Invalidate()`，TJS 的 `_Finalize()` 会先执行游戏脚本 `finalize`，然后才执行 native `Invalidate()`。上游 TJS 在脚本抛错时复位 `IsInvalidating`，并保留 `IsInvalidated=false`，允许正常的异常传播。
3. 清理异常逃到 `TVP_CATCH_AND_SHOW_SCRIPT_EXCEPTION`，由 `TVPProcessUnhandledException()` 调用游戏的 `System.exceptionHandler`。游戏又提示错误并请求退出，`process_events()` 返回继续运行，下一轮重复清理尚未失效的同一窗口。

现在清理函数本地捕获窗口脚本失效异常，继续释放仍注册的原生窗口；已经注销的原生窗口不再次失效，避免重复释放。原生子对象清理抛错时仍释放残留 form；没有注册表进展时结束本次清理，避免盲重试。`TVPWindowWindow::InvalidateClose()` 先通知 native `NotifyWindowClose()` 再释放 form，避免后续析构访问已释放的 form。

该修复只作用于退出时的窗口释放边界，不改变 TJS 通用 `finalize` 语义，也不屏蔽剧情执行中的缺插件问题。`TVPShowSimpleMessageBox()` 在 Switch 上只写日志，没有阻塞等待弹窗；本次循环不能归因于弹窗等待。正常进程级系统 uninit 仍保持原行为，重建路径继续负责会话资源清理。

## 视频音频：已确认的供给缺口与通用修复

本地只读提取的 `build-scene-freeze/actual-movie-main.wmv` 对应真实游戏的 `羽.wmv`，与本轮 fixture 的 `romfs/sample.mp4` SHA-256 一致。ffprobe 确认其扩展名虽然为 `.wmv`，实际容器是 MP4 系列，视频为 H.264 1280×720，音频为 AAC 44.1 kHz 双声道，总时长约 50.085 秒。另一个 `actual-movie-up.wmv` 对应 `羽UP`，不是本轮完整播放 fixture 的输入。媒体只保留于本地排查目录，不加入提交的回归资源。

`actual-movie-packet-layout.json` 显示音视频包成组交替：起始约 16 个视频包、22 个 AAC 包，后续多次约 15 个视频包和 21 个 AAC 包。15 帧视频在 30 FPS 下覆盖约 500 ms；21 个 AAC 包按每包 1024 个采样帧计算约 488 ms。这里只用包分组解释供给时序，不将包 PTS 首尾差当作精确输出音频时长。

`SwitchMovieOverlay` 原有四个 16 KiB PCM block，最多同时提交三个，另一个保留供旋转复用。S16 双声道 44.1 kHz 下，设备队列仅覆盖约 `3 × 16384 / (44100 × 2 × 2) = 279 ms`。旧视频定时等待没有持续调用音频补队列；等待视频包组期间，即使 PCM ring 还有已解码数据，设备也可能先播放完三块并断流。该供给缺口由真实包布局和确定性 host 模拟复现；当前游戏视频音频正常的最终验收来自后续用户反馈，并结合下文完整播放运行证据。

本轮实现位于 `src/core/visual/sdl2/SwitchMovieOverlay.cpp/.h` 和 `MovieAudioQueue.h`，适用于共用 FFmpeg 视频播放路径：

- 视频定时等待期间继续补充已解码 PCM；一次补给填满可用设备槽位。缓冲区数量和容量不变，仍保留一个旋转 block，遵守消费回调归还的所有权。
- 下一个音频包尚未到达且设备队列将耗尽时，允许提交已有的完整采样帧尾段；这种非 EOF 尾段不标记 EOS。正常 pause/stop 保留原有控制含义。
- PCM ring 满时按背压等待并继续补队列，不丢掉当前解码帧剩余 PCM。
- demux EOF 后排空音频 decoder 的延迟帧，再排空 swresample 尾部；处理 send/receive 的 EAGAIN 和无进展边界，最后发送 PCM 尾部并保持原有有限等待。
- 每次新的播放使用新的音频 source；关闭时先同步销毁旧输出，再清空 PCM 和回调额度，防止旧播放的完成回调污染新播放。pause/resume 继续使用当前 source。
- 低频 `[movie-audio]` 日志记录提交、完成、队列、PCM 及 `buffered-starve`，供实际运行复核。该计数表示补给时发现设备队列空且已有 PCM，不等于完整的听感或音画同步测量。

该改动没有以扩大队列容量掩盖等待期间不补给的问题，也没有调整独立的启动时钟偏移或重新定义音画同步。

## 已完成的测试及边界

`build-scene-freeze/host-final-tests.log` 记录完整 host CTest **14/14 通过**，包括此前音频生命周期/丢唤醒、E-mote、位图和菜单回归，以及本轮 `layer_raster`、`movie_audio_queue`、`window_cleanup`。

| 测试 | 已验证内容 | 不能替代的验收 |
| --- | --- | --- |
| `layer_raster_test` | 生产 Raster 算法与独立像素参考比较；正负 pitch、边缘、padding/guard、源不变、原地复制及参数边界。 | 实际游戏完整转场和所有插件契约。 |
| Raster NRO fixture | `raster-runtime.log:77–192` 完成 **17 项检查**；真实原生注册、大小写重复加载、内置插件探测、不可卸载、像素和输入校验均通过，随后 native 动画持续运行。 | 用户原来的剧情路径。测试刻意捕获的非法参数异常不是运行失败。 |
| `window_cleanup_test.py` | 直接提取生产 cleanup 和 `InvalidateClose` 编译，覆盖正常清理、脚本 finalize 失败、原生子项失败、无 owner、别名 form 和无进展边界。旧源码同一测试失败于 `extractTrigger is already invalidated`，当前代码通过。 | 完整 TJS 引擎销毁和实际启动器重建。 |
| `movie_audio_queue_test` | 生产队列 helper 的 FIFO、背压、回绕、回调额度、pause/stop、EOF 尾部、decoder/resampler 排空和输出重建；真实 44.1 kHz 包组时序中，视频等待持续补给不会丢失已有 PCM。 | 实际 FAudio 调度、连续听感、真实媒体完整播放和音画同步。 |
| 视频音频 NRO fixture | 第二轮 `movie-runtime.log` 无脚本异常，真实影片素材完整自然播放至 EOF，540 个 PCM block 全部消费，随后关闭输出、codec 和 storage，显示绿色终态。 | 用户连续听感、音画同步和真实游戏原故障路径。 |

证据目录还保留 `raster-runtime-pass.png`、原始卡死截图和旧窗口清理失败基线。Raster 的 17 项检查以 `raster-runtime.log` 为依据，截图仅作为现场辅助记录，不作为“没有 modal 的完整波纹渲染”验收。没有用 fixture 结果覆盖或宣称已恢复用户游戏存档。

## 构建、正式部署与实际运行

15:05 候选构建日志为 `build-scene-freeze/candidate-final-build.log`。核验到以下两个文件一致：

- `build-switch/krkrsdl2.nro`
- `build-scene-freeze/candidate-final.nro`

构建时间为 **2026-09-27 15:05:23**，大小 **28,742,641 bytes**，SHA-256：

`4497CCE555F97CD67EE48BA2221A32C62693E57E3E8CE2DBE4BE86ABA403E907`

正式部署记录见 `build-scene-freeze/deployment-final.json`，部署时间为 **2026-09-27 15:17:10 +08:00**。以下两个目标均已替换为上述 15:05:23 构建、28,742,641 bytes、`4497CCE...` 完整哈希的 NRO：

- `out/krkrsdl2.nro`
- `D:\KRKR-ns-tools\emulator\publish\portable\games\krkrsdl2.nro`

当前模拟器实际运行继续保持 `build-scene-freeze/candidate-final.nro`，没有为部署中断用户游戏。上一正式包哈希为 `5EF90E4E0DD15706E2552653A209FE79A102B3C13950A3BB49B1ADCD4665FD4A`。独立 fixture 会替换 RomFS 测试脚本，不将其整体 NRO 哈希混作生产包部署记录。该部署尚未更新 GitHub Release，源码修复和测试仍为未提交改动。

以该 15:05 候选原生代码打包的 `build-scene-freeze/movie-audio-fixture/movie-audio.nro` 已完成第二轮干净运行，最终证据为 `build-scene-freeze/movie-runtime.log`：

| 行号 | 结果 |
| --- | --- |
| 476 | demux 到达 `End of file`。 |
| 477 | `decoder-eof=1 resampler-frames=0 error=0`，decoder 排空完成，没有 resampler 尾部剩余或错误。 |
| 479 | 最终画面达到 frame 1500。 |
| 484 | `pcm=0 queued=0 submitted=540 bytes=8835072 completed=540 buffered-starve=0`。全部 540 个提交 block 已消费，没有 PCM 或设备队列剩余。 |
| 485、494–503 | `stop` 通知后释放输出、codec、format、AVIO 和 storage，最终 `STATUS unload`。 |
| 504 | `COMPLETE natural EOF; polls=204`。 |

`movie-complete-dpi.png` 已核对为绿色终态，与日志的 fixture 完成结果一致。该轮没有第一轮的脚本 poll 异常；完整 PCM 量仍为 `8835072 / 4 / 44100 = 50.085442 秒`，与媒体时长一致。这些证据验证完整自然播放、队列消费和关闭流程，不能单独证明声音听起来连续或音画同步。

第一轮 fixture 曾错误轮询不存在的 `VideoOverlay.status` 属性；校正为记录 `onStatusChanged` 通知后，第二轮干净运行通过。最终影片证据只引用保留的 `movie-runtime.log`，不将此前脚本校验失败列为通过。

真实《魔女的夜宴》从存档继续运行后，用户回复 **“现在正常了”**，这是当前剧情恢复正常的验收依据。`build-scene-freeze/game-runtime.log:1017` 确认实际加载 `layerexraster.dll`；该运行快照没有 `layerExRaster plugin not loaded`、脚本异常或退出清理循环。`scene-accepted.png` 记录当前显示药瓶的实际游戏画面；单张截图没有记录完整剧情经过，不据此推断已越过某个卡死点或到达后续教室场景。

用户随后进一步回复 **“视频音频也正常了”**，因此当前实际游戏的视频音频正常也列为用户确认通过，取代此前不方便试听的待确认状态。日志和 fixture 验证完整自然播放、全部 PCM 消费及正常关闭，用户反馈补充了当前游戏实际听感正常的证据。

验收范围限定于本轮模拟器、当前实际游戏及上述测试。物理 Switch 尚未验收，没有完成所有素材/其他游戏的同步精度测量；本轮也没有另行完整验证真实游戏的异常退出和连续跨游戏启动，窗口清理的通用保障由生产函数回归测试覆盖。不将当前用户确认扩展为所有游戏、全部退出路径或物理设备验收。

此前 Riddle Joker 卡顿和自然退出问题的最终记录见 `RUNTIME_EXIT_LAG_20260927.md`；本轮的用户确认、部署和影片运行证据以本文记录为准。

本轮两份大诊断日志已压缩并逐字节校验，移除重复原日志后释放约 453 MiB，13 个存档及配置备份保留。另有约 166 MiB 的临时测试包、旧候选和中间截图列入 `build-scene-freeze/cleanup-plan.json`；批量递归删除和明确文件路径删除均被工具自动检查拒绝，返回 `blocked by policy`，没有给出详细原因。这部分尚未清理，状态记录在 `cleanup-verification.json`，没有绕过拒绝继续删除。
