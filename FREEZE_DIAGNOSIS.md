# 剧情长时间推进卡死与选项文字修复（2026-09-12，P63）

本轮在 MTP 上只读取真机日志，没有核对或写入真机 NRO。用户确认真机使用当前构建。问题可以发生在普通剧情推进中，快进只是更快触发大量图像更新；测试按一次按钮切换快进，不依赖反复点击。

## 已定位的问题

### 大位图分配失败

真机两次《星光咖啡馆与死神之蝶》日志均出现 `Cannot allocate memory for Bitmap`。最新一次的请求是 11,059,248 字节、1920×1440，调用经过 `updateregion.tjs`、`affinelayer.tjs`、剧情绘制。脚本错误被现有异常保护忽略后，剧情可能停住，后台心跳仍继续。

在模拟器用未改变分配策略的诊断版复现了同类错误。重试失败时请求 17,651,760 字节（2028×2176），位图实际持有约 265 MiB；`mallinfo` 显示通用堆约 3.17 GiB，其中约 2.38 GiB 已空闲，却无法满足这次连续分配。结合通用堆布局和重复大图写时复制路径，证据指向堆碎片化，并非位图持续持有了全部可用内存。

启动日志的 `process-memory used` 包含进程已经保留的堆，不能等同于活跃对象占用。旧版只报告这一数字时无法区分碎片和泄漏。

### 文字对象销毁破坏其他字体

加入排版回归后，又复现了一处独立崩溃：`TextRenderBase` 释放自己的 `FreeTypeFontRasterizer`，其析构函数却执行全局 `FT_Done_FreeType`，连其他仍活跃的字体 face 一起销毁。接下来的普通 `Layer.drawText` 在 `FT_Request_Metrics` / `FT_Set_Pixel_Sizes` 访问失效 face，模拟器报告空地址访问。

这处错误在单个共享光栅器的实现中不易暴露，移植后的独立文字排版实例会触发它。不能据此断言全部真机卡死都由它造成，但它是本轮明确复现并修复的通用生命周期错误。

### 选项文字垂直位置

`TextRenderBase::Clear()` 缺少 `valign == 0` 的居中分支，把它当成顶部对齐。对照本地 krkrsdl3 的排版实现，补回 `(BoxHeight - Ascent()) / 2`，使用当前实例所选字体的 ascent。顶部和底部模式保持原语义。用户已经用实际游戏截图确认文字在选项框内居中。

## 实现

- Switch 大位图使用独立的 64 MiB 内存区，按 256 KiB 页分配；适用请求为 256 KiB 至 32 MiB。空闲相邻页可以合并满足不同大小的图像，避免通用堆的小对象进入刚释放的大图空间。固定元数据、互斥保护；活跃像素不搬移，原有写时复制和越界哨兵保留。
- 最多保留 128 MiB **完全空闲**的内存区；正在使用的内存区可能还有额外空闲页，因此 128 MiB 不是整个位图池上限。内存压缩事件释放完全空闲区。过小、过大以及内存区扩展失败的请求仍可走原始分配器。
- 位图分配记录保留实际容量，使用 `size_t` 计算附加空间。新增每 10 秒内存快照及分配失败、压缩重试日志，区分活跃位图、通用堆已用/空闲、位图池保留/空闲页和复用次数。
- FreeType 光栅器仅销毁自己的 face；全局库改在引擎 `CLEANUP` 阶段释放，晚于脚本对象和共享光栅器释放。
- 选项居中修复作用于公共排版器，没有修改游戏脚本或按游戏名分支。

## 验证

1. 本地 5 项 CTest 全部通过：位图池、菜单公共工具、位图桥接、E-mote GL 和 CPU。位图池检查包括相邻空闲块合并、不同图像大小、清理时保留活跃数据，以及四线程共 6,000 轮分配/写入/释放。
2. 模拟器运行独立 ARM 引擎测试：**22,813 项校验全部通过**。包括此前字体、六种存档、PSB、固实归档校验，新增六个字号的三种垂直对齐、16 轮临时文字对象销毁后其他对象继续绘制，以及 300 轮 1920×1440 大位图写时复制。最终报告和正常脚本结束标志均存在。
3. 修复居中之前的 NRO 使用同一排版测试，明确失败于 `centered option has a vertical inset`。修复字体生命周期之前的 NRO 通过排版和位图复制后，在普通图层文字绘制处崩溃；最终版通过该路径。
4. 与 P62 的完整测试结果比较，31 个字号的文字宽度一致，六种存档文件字节完全一致。
5. 真实游戏的位图内存区版本运行约 18 分钟，包含剧情快进、暂停/失焦及选项画面，并非 18 分钟连续快进。95 个内存快照中无位图分配失败；最后累计 13,646 次内存区复用，通用堆最高约 1.61 GiB，位图池最高保留 512 MiB。测试没有覆盖整个游戏，不能推断任意时长的内存上界。这一长跑发生在字体生命周期补丁之前，最终版另外通过上述完整引擎回归。
6. 选项居中由用户截图确认。模拟器失焦/静止页面也可能出现旧心跳诊断的 `MAIN THREAD STALLED`，需要结合后续剧情、输入响应和异常判断，不能单凭它认定死锁。真实游戏仍存在既有音频兼容异常，本轮没有声称全部兼容问题已解决。

真机修复版尚未完成长期运行复验。建议分别进行正常推进、一次切换快进后长时间推进，以及菜单/选项反复进出；若再次停住，保留同次启动日志用于判断位图分配、字体还是其他路径。

## 本地产物与证据

正式 NRO 为 `build-switch/krkrsdl2.nro`，27,982,937 字节，SHA256：

```text
6555294E53D2DE19C02A1F2576EC2F8CFC28C7FD3228C4F6FAC41979C069484A
```

其 RomFS 保留正常游戏启动器。测试 NRO 的 RomFS 才包含合成数据，不应部署到游戏目录。

测试结束后，模拟器的 91 个存档文件已恢复到测试前备份，逐文件 SHA256 一致；本轮测试产生的进度另存于 `emulator-saves-after/`。本地模拟器的单一 NRO 入口已同步正式产物，真机文件未写入。

本地 `.zcode/hardware-freeze-20260912/` 保存：

- 真机原始日志 `krkrsdl2_debug_1789208641.log`、`krkrsdl2_debug_1789208994.log`。
- `emulator-diagnostic.log`：分配失败前后的堆和位图计数；原 P62 NRO 为 `before-fix.nro`。
- `emulator-arenas-final.log`：内存区版本真实剧情运行；`arena-current-state.png`：修复后的选项画面。
- `regression-before-text.log`：居中测试明确失败。
- `font-lifetime-crash.log`：字体库提前释放的模拟器堆栈；`regression-before-font.log`：对应引擎记录；该 NRO 为 `before-font-lifetime.nro`。
- `regression-lifetime-final.log`、`regression-lifetime-final-results/`：最终完整测试日志和存档输出。

复现回归测试：

```powershell
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
python tests/build_menu_perf_fixture.py --nro build-switch/krkrsdl2.nro --output-dir .zcode/freeze-regression
# 在模拟器运行生成的 menu-perf.nro，结束后保存 perf-test 目录，再比较：
python tests/compare_menu_perf_results.py .zcode/menu-perf/after-final-results .zcode/hardware-freeze-20260912/regression-lifetime-final-results --after-checks 22813
```
