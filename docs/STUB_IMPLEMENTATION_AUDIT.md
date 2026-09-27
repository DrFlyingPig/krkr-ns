# 空占位实现专项审计

审计日期：2026-09-27。主线起点 `dc284da371ce50c71db62b9654bf8127b5c52ed9`；Kirikiroid2 参考 `d1c2b1259423542c893e0b65eaeb46c848848f2b`；krkrsdl3 参考 `c014d307b02a2ee8f10b69b39ba8c31e8ee254d4`。参考树在 `.zcode/`。本文记录本次检查确认的缺口，不表示所有空函数已穷尽或所有游戏已验收。

## 检查方法

对公开接口追踪 **脚本注册 → 平台条件 → 实际后端 → 状态/回调**，同时检查 `sources.txt` 与 CMake。重点是有名称且返回正常、实际未执行动作或返回固定值的接口。只搜索 TODO、同名函数或空函数体会混入静态链接锚点、合法默认事件、禁用后端和平台差异。

XP3 content/extraction filter 和四个 Layer 绘图方法已在前一轮完成专项运行验证；本轮重新检查到它们已有实际实现，不将其继续列为空桩。后续实施顺序仍见 [KIRIKIROID2_PORTING_PLAN.md](KIRIKIROID2_PORTING_PLAN.md)。

## 本轮已修复

| 项目 | 原行为与影响 | 当前行为及边界 |
|---|---|---|
| 脚本 MenuItem.remove | `children.remove(index, 1)` 将下标当成值；真正子项仍留在数组，parent 却被清空。删除外部子项还会清掉它真正的 parent | 改用 `erase(index)`，仅成功删除且 parent 指向当前节点时断开 parent；专项覆盖中间/首末删除、重复删除、非子项、insert 后删除、null/void |
| 原生 MenuItem.SetIndex | setter 完全空，读取仍为旧顺序 | 在兄弟数组中移动节点、检查边界、刷新 children 缓存。移动位置的参考是 [VCL MenuIndex](https://docwiki.embarcadero.com/Libraries/Sydney/en/Vcl.Menus.TMenuItem.MenuIndex)；Kirikiroid2 这一函数本身也是空桩 |
| 原生 MenuItem.shortcut | setter 丢弃字符串、getter 恒空 | 保存并读回字符串；未实现快捷键规范化、键盘响应或系统菜单界面 |
| 原生 MenuItem 错误与生命周期 | Construct 忽略父类失败；RemoveChild 释放引用后仍写 native 实例 | 传播失败，检查空 native/插入边界/非子项，先断开 parent 再 Release |
| VideoOverlay.audioVolume | 后端有真实控制，但脚本 wrapper 只开放 Win32，Switch 设置无效、读取为 0 | Switch 独立通路使用引擎线性 `0..100000`；Win32 原 DirectSound 衰减转换保持原行为；现有后端负责限幅和 FAudio 音量 |
| VideoOverlay.position 读取 | 后端 GetPosition 不写输出，脚本一直读 0 | 按现有固定 FPS 与最新发布帧的零起始索引计算毫秒；不是完整 PTS 时钟或 seek 实现 |
| 视频状态读取 | 倍率返回 0、enabled track 返回 -1，与正在播放的后端矛盾；混合背景 wrapper 存在未初始化读取 | 返回后端实际状态：固定倍率 1、居中声像 0、当前唯一公开轨序号 0/无轨 -1、内部 EOF 停止边界、实际不透明/黑背景；关闭后的背景值初始化为 0。轨由 av_find_best_stream 选择，不保证是容器第一轨 |

原生 MenuItem 在 `ScriptMgnIntf.cpp` 中仍未注册，正常游戏使用脚本模型。原生修复仅通过源码/构建检查，不能把脚本测试结果作为原生菜单运行验收，也不能称为 MenuItem 整项完成。本轮不改变注册策略，避免再次把游戏自带菜单对象传入只接受 native 实例的树。

## 已确认但尚未补齐的高影响缺口

以下影响是源码调用链推断，除本轮专项包外未在新游戏中逐一重现。

| 优先级 | 接口/位置 | 触发条件和风险 | 参考及下一步 |
|---|---|---|---|
| 高 | `SwitchMovieOverlay.h` 的 SetPosition/SetFrame | 游戏设置 position/frame 或 `setSegmentLoop`。`VideoOvlImpl.cpp` 到循环末帧后调用空 SetFrame 并返回，后续更新反复进入该分支，可能定格并重复循环事件 | K2 `KRMoviePlayer.cpp` 转交 SeekTime。需要 demux seek、codec flush、音频队列清理、帧时间/事件重置；仅记住属性无法修复 |
| 高 | `SwitchMovieOverlay.h` 的 SetPlayRate/SetStopFrame | 脚本倍速被静默忽略；自定义停止帧是尚未实现的内部/插件接口，当前 TJS 不暴露 stopFrame 属性。读取实际默认值的本轮修复并没有实现这些 setter | K2 `KRMoviePlayer.cpp`；KRKRZ `movie/win32/dsmovie.cpp`。要完成停止事件和音画时钟语义 |
| 高 | `emoteplayerclass.cpp:1378` setColor | 人物染色/透明度设置只有 TODO，依赖颜色隐藏或淡出的游戏可能残留角色或颜色错误 | K2 公有树不含该插件；补充参考 `upstream-krkr2/cpp/plugins/motionplayer/manual.tjs:160`、`EmotePlayer.cpp:401`。需贯穿渲染颜色/alpha，不能只保存属性 |
| 高 | `emoteplayerclass.cpp:1553` setTimelineBlendRatio；fadeIn/OutTimeline | 混合权重为空；fade 直接播放/停止，忽略 time/easing，表情与动作渐变突变 | 补充参考 `motionplayer/PlayerTimeline.cpp:294,318,331`；需核对当前 timeline 更新机制和混合顺序 |
| 高 | `emoteplayerclass.cpp:1437,1449` skip/pass | 调用后不推进动画时钟或释放同步等待，可能破坏快进和同步流程 | 补充参考 `motionplayer/EmotePlayer.cpp:800,815`，先核对当前同步状态机 |
| 中 | `emoteplayerclass.cpp:1460` playTimeline flags | Parallel/Difference 参数未参与执行，公开常量并不代表对应能力已实现 | 对照实际 startTimeline 调用与 motionplayer 参考，补混合/并行动作规则 |
| 中 | `emoteplayerclass.cpp:490,1357` assign | 适配器/播放器复制无操作，调用后保留旧状态 | 必须确认资源、变量、timeline、目标层的复制与所有权契约 |
| 中 | E-mote initPhysics、setOuterForce、startWind/stopWind、outline | 多处仅 TODO 或忽略设置，物理/外力/轮廓效果缺失 | 公有 K2 无实现证据，krkrsdl3 也有同类欠实现；不能靠插件名字宣称完整 |
| 中 | `TVPWindow.h:424,449` enableTouch/触摸点轮询/阈值 | getter 恒 false/0，而 SDLApplication 已发送真实触摸事件。轮询式脚本看不到手指，启停状态与事件不一致 | KRKRZ WindowFormUnit/TouchPoint 状态；需按窗口维护点 ID、坐标与生命周期，再映射到游戏坐标 |
| 中 | `SwitchMovieOverlay.h` 声像、轨选择/禁用 | balance 设置无效，多轨容器只开放当前一个可解码轨，无法切换语言或真正禁用音轨 | K2 KRMoviePlayer；其 DisableAudioStream 自身也 TODO。区分继承欠实现与本地遗漏 |
| 中 | `compat-patches/system/win32dialog.tjs` | execute/show/modeless 恒 null，assign/store/getNumber/getItemRect 等无实际内容。依赖插件设置/确认/编辑界面的游戏无法完成交互 | 需按脚本可见模板与返回结果实现 Switch 边界 UI，不能仅让 class 探测通过 |
| 中 | `k2compat_reinstall.tjs:37` loadPlugin/unloadPlugin/autoLoad/isLoaded | load 不加载、unload 不卸载，isLoaded 无条件 true，可能让游戏进入实际上缺少成员的插件路径 | 应对照真实 Plugins 注册表与游戏 namespace 契约；当前不贸然替换游戏自己的包装链 |
| 中 | `SystemImpl.cpp:362` shellExecute；`:982` system | 非 Apple 且带参数时 shellExecute 无动作却返回 true；system 命令调用被禁用却返回 0。游戏不能进入失败回退 | Switch 外部程序限制可接受，但返回值应表达失败；与真实平台边界实现分开处理 |
| 中 | `TextRenderBase.cpp:563,591` setOption/keyWait | setOption 丢弃配置、keyWait 恒空；依赖排版选项/等待标记的脚本可能偏离行为 | krkrsdl3 deserialize 本身不读取字段、keyWait 同样空，属继承欠实现；K2 私有版本无公开源码，需真实探针确认契约 |

其它菜单缺口仍包括脚本 popup 恒 0、addHook/removeHook 无行为、Window.menu 使用共享根、index/单选组状态未与完整树语义绑定；native popup 无界面却返回 1。原生树跨父 Add/Insert、祖先环、直接 invalidate 子项的父树清理也未验收。补真实菜单系统时需要一次解决树、事件、快捷键、每窗口根和 UI。

## 功能缺失与合法空函数的区别

- **ZIP/TAR 是缺少实现，不是现有函数空桩。** `StorageImpl.cpp:939` 的 creator 链为 Susie → 7z → XP3；K2 `StorageImpl.cpp:588` 还支持 ZIP/TAR。重命名成 `.xp3` 的 ZIP/TAR 包也不能因此被当前 XP3 识别。FFWaveDecoder 同样没有接入，影片能解某些 FFmpeg 格式不等于 WaveSoundBuffer 能解相同音频。
- **fftgraph.drawFFTGraph 的空函数与 K2 公有实现一致。** 对可视化仍有局限，但不属于本移植新增回归。
- **静态插件 `krkrsdl2_link_*` 空函数是合法链接锚点。** 它们保留 auto-register translation unit，不是插件功能本身。
- **旧音频后端不等于当前运行路径。** `src/core/sound/sdl2/WaveImpl.cpp` 不在实际 source list；OpenAL 实现在当前 FAudio 宏下排除。不能用它们的空提交/3D 方法证明默认 Switch 音频无效。
- **NullAudioDevice 是条件可达缺口。** 仅 `-audiodevice=null` 路径使用；采样进度和 buffer completion 为空，可影响依赖音频结束的等待。默认 FAudio 不走此路径。
- **ZoomRectangle 是禁用视频路径中的遗漏。** 当前 TVPWindow 默认空，K2 MainScene 有缩放；本项目唯一调用位于 Win32 视频宏内，Switch 当前不执行，不列为 Switch 画面主因。
- **父类默认事件、无 OS HWND、禁用的 Win32 消息等必须逐调用判断。** 固定系统能力值和默认无操作不能一概改成模拟成功。

## 验证与产物

### 运行缺陷修复与正式部署：2026-09-27 13:24

空占位排查过程中发现真实游戏返回标题持续卡顿及退出音频等待黑屏，已追加 E-mote 有限播放状态、音频 dispatcher 重建、短声音 EOS 以及播放线程漏唤醒修复。详细证据见 [RUNTIME_EXIT_LAG_20260927.md](RUNTIME_EXIT_LAG_20260927.md)。最终 host 11/11 通过，真实 Riddle Joker 返回标题后流畅，退出无需点击黑屏自动回到初始游戏库。

13:17:33 构建的正式 NRO 已于 13:24:53 同步到 `build-switch/`、`out/` 和模拟器固定文件名，三份 SHA-256 均为 `5EF90E4E0DD15706E2552653A209FE79A102B3C13950A3BB49B1ADCD4665FD4A`。6 个 SD 补丁与源码匹配，临时测试入口已清理。此结果不代表全部占位接口或所有游戏已完成验收。

### 主版本部署补记：2026-09-27 12:10

专项验证阶段使用 `--no-emu-copy`，没有更新主版本。用户指出模拟器仍显示旧版后，已运行完整 `build_nro.sh`，部署模拟器游戏目录 NRO 与全部 6 个运行兼容文件，并同步 `out/krkrsdl2.nro`。

`build-switch/krkrsdl2.nro`、`out/krkrsdl2.nro`、`D:/KRKR-ns-tools/emulator/publish/portable/games/krkrsdl2.nro` 的时间均为 `2026-09-27 12:10:00`，大小均为 28,734,449 bytes，SHA-256 均为 `2C4D092F1313E022E890157DC31C4E5C61CD4FDF5EC3ECAD9D24B5CFA8A7747E`，与专项测试使用的代码产物一致。6 个兼容文件逐个与源码 SHA-256 一致，退役 `motion.tjs` 不存在。验证清单保存于 `build-switch/deployment-verification.json`。

旧主版本和原部署兼容文件已备份至 `.zcode/deployment-backups/20260927-120954/`。没有关闭用户已有模拟器进程；已经加载的旧 NRO 需要重新加载才会使用新文件。本次是模拟器部署，不表示物理 Switch 部署或新增多游戏运行验收。

### 部署之前的专项验证记录

- `build_nro.sh --no-emu-copy`：Switch 重新编译四个改动的原生 translation unit，链接与 NRO 打包通过。候选在 `build-switch/krkrsdl2.nro`，28,734,449 bytes，SHA-256 `2C4D092F1313E022E890157DC31C4E5C61CD4FDF5EC3ECAD9D24B5CFA8A7747E`。本轮没有替换 `out/krkrsdl2.nro` 或模拟器游戏目录主版本，两者仍为 `019F0AFB73029D47BE5F4AE40D224CFC2052E740DBE859403A619EAFCE37EC58`。
- 主机 `ctest --test-dir build-tests -C Release --output-on-failure`：6/6 通过。干净环境使用仓库 SDL2 构建；其 MSVC 内置 GLES2 头没有 PFNGL 核心函数类型，测试配置预包含仓库 ANGLE 的 GLES2 头解决，未修改游戏渲染源码。
- `menu_contract` 专项：Ryujinx 新日志 `krkrsdl2_debug_1790481596.log`，`COMPLETE 15 checks; script model only`；复制日志与截图在 `build-menu-contract-fixture/`。首版测试包遗漏自动执行的 namespace reinstall 脚本，已修正打包依赖后重跑通过。
- `movie_contract` 专项：由 ffmpeg 生成 3 秒 H.264/AAC 测试片，Ryujinx 新日志 `krkrsdl2_debug_1790481859.log` 得到 `COMPLETE 18 checks`。覆盖音量限幅/播放中修改、初始/播放/暂停/恢复位置、轨序号、默认状态、关闭后的读取；日志确认真实 FAudio 输出和解码线程已启动。复制日志与截图在 `build-movie-contract-fixture/`。读取位置按最新发布帧计算，保留现有固定 FPS 的约 66 ms 解码提前量；最后一帧起始时间为 2966 ms，并不代表 EOF 自动到 3000 ms。该测试没有验收 EOF、VFR/PTS 时间精度、多轨选择顺序、暂停期间的音频停播或真实音量响度。
- 原生 MenuItem 当前不可从普通游戏构造；本轮只完成编译检查。物理 Switch 与真实游戏回归仍未覆盖，不能据此声称所有空占位已经补齐。

复现专项包：

```powershell
python tests/build_core_port_fixture.py --nro build-switch/krkrsdl2.nro --output-dir build-menu-contract-fixture --fixture menu_contract
ffmpeg -hide_banner -loglevel error -f lavfi -i testsrc2=size=320x180:rate=30 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 3 -c:v libx264 -preset ultrafast -pix_fmt yuv420p -c:a aac -ac 2 -movflags +faststart -y build-movie-contract-fixture/sample.mp4
python tests/build_core_port_fixture.py --nro build-switch/krkrsdl2.nro --output-dir build-movie-contract-fixture --fixture movie_contract --media build-movie-contract-fixture/sample.mp4
```
