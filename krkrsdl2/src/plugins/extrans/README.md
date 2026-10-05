# extrans.dll

KAG 的转场（transition）提供者插件。与其余内置插件不同，它不注册任何脚本类，
而是在模块加载时用 `TVPAddTransHandlerProvider` 把一组**按名字解析**的转场
处理器注册进引擎；KAG 侧的 `@trans method=<名字>`、标题自己的转场表以及读档/
自动存档画面都是按名字请求的。

来源：krkrsdl3 `plugins/extrans/`（`LICENSE.krkrsdl3`）。除 include 集合与
`TJS_N` → `TJS_W` 外未改动参考实现，包括各参数的默认值与取值范围检查。

## 成员（转场名）

| 阶段 | 名字 | 参考文件 | 关键选项 |
|---|---|---|---|
| 1 | `wave` | `wave.cpp` (394 行) | `time`（必需，<2 视为 2）、`maxh`=50、`maxomega`=0.2、`bgcolor1`/`bgcolor2`=0、`wavetype`=0（0 两端细、1 起端细、2 末端细） |
| 1 | `mosaic` | `mosaic.cpp` (467 行) | `time`（必需）、`maxsize` |
| 3 | `turn` | `turn.cpp` + `turntrans_table.cpp` (584 + 4530 行) | `time`、`rule`（卷动方向/形状） |
| 2 | `rotatezoom` / `rotatevanish` / `rotateswap` | `rotatetrans.cpp` + `rotatebase.cpp` (542 + 730 行) | `time`、`centerx`/`centery`、`rwidth`(16/32/64/128)、`roundness`、`speed`、`maxdrift` |
| 3 | `ripple` | `ripple.cpp` (1738 行) | 同上，另有 `ripple` 专用的漂移/精度参数 |

引擎侧解析在 `external/krkrz/visual/TransIntf.cpp` 的 `TVPFindTransHandlerProvider`：
名字找不到时回落到内置 `crossfade` 并记一条 `[trans] missing transition handler`，
所以缺名字不会让游戏中断，只会看到不同的（较弱的）转场效果。

## 引擎差异（本移植）

- 参考里的 `cpu_types.h`、`TVPMsg.h`、`TVPTrans.h` 在 krkrsdl2 中分别是
  `tjsCommHead.h` 已有的整型定义、`MsgIntf.h`、`transhandler.h`
  （+`TransIntf.h` 的注册入口、`tvpgl.h` 的混合核）。
- 混合核 `TVPFillARGB` / `TVPConstAlphaBlend_SD[_a|_d]` 在本引擎是函数指针，
  由 `visual/gl/blend_function.cpp` 初始化，引擎自身的 crossfade 用的是同一组。
- 会话结束走 `TVPUnloadBuiltinPlugins` → ncbind 后置注销回调 → 逐个
  `TVPRemoveTransHandlerProvider`，避免下一局重复注册时命中
  `TVPTransAlreadyRegistered`。

## 验证

见 `docs/K2_PLUGIN_CONTRACTS.md` 与 `docs/KIRIKIROID2_PORTING_PLAN.md`：每个阶段
都要有模拟器 fixture（用 `Layer.beginTransition` 驱动真实转场并读回像素）与
真实游戏会话，日志确认不再出现该名字的 `missing transition handler`。
