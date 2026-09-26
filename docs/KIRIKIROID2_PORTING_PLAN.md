# Kirikiroid2 源码对齐移植优先级

> 建立：2026-09-21。参考基线：本地 `.zcode/upstream-kirikiroid2`（`zeas2/Kirikiroid2`，审计时提交 `d1c2b12`）。
> 本文是 **KRKR-ns 对 Kirikiroid2 的当前权威实施顺序**；`COMPAT_BACKLOG.md` 是旧的 krkrsdl3 对照，不再决定本轮优先级。

## 验收口径

- **源码对齐**：同时核对 C++ 实现、TJS 可见签名、默认参数、回调/状态生命周期，不能以同名空桩代替。
- **构建验证**：Switch 全量 NRO 必须成功链接；现有主机测试不得回归。
- **运行验证**：涉及画面、音视频或归档回调时，最终仍需对应测试包和 Switch/模拟器日志、截图或像素结果。仅构建成功不等于运行验收。
- 平台差异只允许放在边界层；核心脚本语义以 Kirikiroid2 为准。

## 实施顺序

| 顺序 | 移植项 | 完成定义 | 当前状态 |
|---:|---|---|---|
| 1 | XP3 content/extraction filter 完整契约 | `FileName`、流级 context、TJS 六参数 extraction 回调、content filter 三参数及 `[action, context]` 返回、action=1 整文件预取、清理/换游戏不串状态 | **已完成；Ryujinx 契约保护包运行通过** |
| 2 | Layer 公共绘图 API | `blendRect`、`stretchPile`、`stretchBlend`、`affineBlend` 的 C++ 像素实现、两种 affine 重载、TJS 参数与弃用参数语义、像素测试 | **已完成；Ryujinx 像素夹具 12/12 通过** |
| 3 | 原生 MenuItem 语义 | 用原生类恢复父子关系、事件、checked/enabled/visible/radio/groupIndex 等行为，替代只够探测的 TJS 模型 | 待移植 |
| 4 | 音视频控制面 | Kirikiroid2/krkrz 的 FFmpeg 波形解码能力，以及 VideoOverlay 的 play/pause/seek/segment/状态回调语义 | 待移植 |
| 5 | 归档与公开插件缺口 | ZIP/TAR 自动识别；`layerExMovie`、`layerExPerspective`；BPG/PVRv3 等实际有标题命中的格式 | 待移植 |
| 6 | 系统交互 API | 输入框、文件选择、消息框、Pad/触摸映射等脚本可见契约，按 Switch 能力做边界适配 | 待移植 |
| 7 | 私有插件语义深挖 | E-mote 与 TextRender 按方法和状态机逐项验证；以真实标题/探针结果补齐，不能依据插件名宣称对齐 | 持续项 |

## 2026-09-21：第 1、2 项落地范围

### 1. XP3 filter

- `XP3Archive` 增加 Kirikiroid2 的 content-filter 类型与 setter。
- 每个 `tTVPXP3ArchiveStream` 保存独立 `tTJSVariant` context；content filter 在条目打开时得到 `(filepath, archiveName, fileSize)`，返回数组第 0 项决定动作，第 1 项成为 context。
- extraction info 增加 `FileName`，TJS 回调获得 `(hash, offset, buffer, length, fileName, context)`。
- action=1 时，完整条目经过 extraction filter 后转为内存流。
- 清空或切换 `xp3filter.tjs` 时同时清除两个底层 hook，并重置原生 XOR 快路径；存在 content context 时禁止快路径绕过脚本。

### 2. Layer API

- 混合模式与 Kirikiroid2 一致：pile 路径用 `bmAlphaOnAlpha/bmAlpha`，blend 路径用 `bmCopyOnAlpha/bmCopy`。
- `affineBlend` 同时提供矩阵与三点重载；脚本层参数、默认 opacity/stretch type、废弃 hold-alpha 参数告警按上游接口保留。
- `tests/fixtures/core_port/startup.tjs` 增加四个方法的调用和像素断言。

## 本轮验证记录

- `build_nro.sh --no-emu-copy`：通过，产出 `build-switch/krkrsdl2.nro`。
- NRO 大小：28,331,285 bytes；SHA-256：`4FB98B023EC49CE2FC0FB917BEEA53495FDEAD43606543D6572BBE2EBD12DF94`。
- `ctest --test-dir build-tests -C Release --output-on-failure`：5/5 通过。
- 交叉编译产物符号已确认包含 content-filter setter/wrapper，以及四组 Layer C++ 方法。
- `tests/build_core_port_fixture.py` 已把像素夹具封装为修正版 `build-core-port-fixture-v2/core-port.nro`（28,244,509 bytes，SHA-256 `44A54E5C5D472117CEFB4D63ECFCF548A0EA75D717A3D4D7EE5E6FBE98953E0D`）。Ryujinx 日志 `krkrsdl2_debug_1789987281.log` 中四个 Layer 方法的像素断言全部通过，最终为 `[port-test] COMPLETE checks=12 local updates active`；运行时看到的色块是该夹具的预期测试画面。
- `tests/build_xp3_filter_fixture.py` 已生成 `build-xp3-filter-fixture/000_XP3_CONTRACT/contract.xp3`（17,700 bytes，SHA-256 `FCDBCE37BF3376BB3971DE0025980CFE8C2E08171F0FF6E44670D38B3F17E7A5`）及配套 `xp3filter.tjs`（1,407 bytes，SHA-256 `EB8617F68DA5AA0B5CF69ABF9C9D6BF8A4C3786DEE34F04F9C3FB70DE656434E`）。Ryujinx 日志 `krkrsdl2_debug_1789993737.log` 确认 content 三参数、`[action, context]`、逐文件 context、action=1 全量数据与 extraction 六参数均正确，最终为 `PAYLOAD PASS`、`STARTUP PASS`。
- 换游戏会话回归已通过模拟器实测：5 个标题连续启动和退出，每次均重建启动器并成功 `present`；同名字体冲突与首版清理造成的退出黑屏均已消失。该结果只验收字体/重启生命周期，不替代下列两项专项测试。
- 当前主线 NRO 重新部署后，用户确认普通游戏进入与运行正常。第 1、2 项已完成源码、构建、专项运行和主版本回归；物理 Switch 验证仍作为发布前独立步骤保留，不影响本轮两项的 Ryujinx 验收结论。
