# extrans.dll

KAG 的转场（transition）提供者插件。与其余内置插件不同，它不注册任何脚本类，
而是在模块加载时用 `TVPAddTransHandlerProvider` 把一组**按名字解析**的转场
处理器注册进引擎；KAG 侧的 `@trans method=<名字>`、标题自己的转场表以及读档/
自动存档画面都是按名字请求的。

来源：krkrsdl3 `plugins/extrans/`（`LICENSE.krkrsdl3`）。**五组全部已移植**；除
include 集合与 `TJS_N` → `TJS_W` 外未改动参考实现（含各参数默认值与取值检查，
`ripple.cpp` 的 SSE2 分支被上游自己的 `#if _MSC_VER && __x86_64__` 排除在 ARM 之外），
已用 `diff` 逐文件核对与上游一致。

## 成员（转场名）

| 状态 | 名字 | 参考文件 | 关键选项 |
|---|---|---|---|
| 已完成 | `wave` | `wave.cpp` (394 行) | `time`（必需，<2 视为 2）、`maxh`=50、`maxomega`=0.2、`bgcolor1`/`bgcolor2`=0、`wavetype`=0（0 两端细、1 起端细、2 末端细） |
| 已完成 | `mosaic` | `mosaic.cpp` (467 行) | `time`（必需）、`maxsize` |
| 已完成 | `turn` | `turn.cpp` + `turntrans_table.cpp` (584 + 4530 行) | `time`、`rule`（卷动方向/形状） |
| 已完成 | `rotatezoom` / `rotatevanish` / `rotateswap` | `rotatetrans.cpp` + `rotatebase.cpp` (542 + 730 行) | `time`、`centerx`/`centery`、`rwidth`(16/32/64/128)、`roundness`、`speed`、`maxdrift` |
| 已完成 | `ripple` | `ripple.cpp` (1738 行) | 同上，另有 `ripple` 专用的漂移/精度参数 |

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
- **与上游的唯一行为差异**：`Unregister*TransHandlerProvider` 加了空指针守卫并在
  Release 后把静态指针置空。ncbind 的后置注销回调在**每次**会话结束都会执行，与
  该会话是否链接过本插件无关；上游是 DLL，回调只在卸载时跑一次，所以无条件
  `Release` 没问题，而静态链接下第二局会对已释放的指针再次 Release。带守卫后，
  未链接过的会话结束是空操作，链接过的会话结束后指针归零，下次 Register 重新分配。

## 验证

模拟器 fixture `tests/fixtures/extrans`（`python tests/build_core_port_fixture.py
--fixture extrans`）44 项：探测/link/不可卸载，每个 provider 的算法签名（wave 的
边界背景色与 `maxh` 上限、mosaic 的 ≥3 像素块、turn 的 bgcolor 折叠边缘 +
同帧两张源图、rotate 系的几何重映射、ripple 的奇偶位移），以及全部以 src2 收尾。
`TransIntf.cpp` 现在对每个缺失的转场名各记一条日志，所以日志中不出现某个名字的
`missing transition handler` 即为该名字确实解析成功。真实游戏会话仍待用户复测
（LimeLight / 千恋万花的转场表、fate stay night 的 wave/mosaic）。
