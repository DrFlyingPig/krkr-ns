# 原生模块与接口说明

更新：2026-10-01。本文件合并 AlphaMovie 与文件移动说明。下述构建哈希和检查数量标明其历史日期；当前能力与新增专项的真机边界见 [兼容计划](KIRIKIROID2_PORTING_PLAN.md)，最新包身份与用户复测见 [运行记录](RUNTIME_NOTES.md)。

<a id="alphamovie"></a>

## AlphaMovie 原生支持与验证记录

2026-09-26。真实 AJPM 解码器和 AlphaMovie 原生脚本接口已接入 Switch 引擎，NRO 已编译部署。主机解码、原 DLL 行为对照和模拟器原生集成检查已完成；用户已确认启动、剧情运行、快速读档和设置界面正常。物理 Switch 性能仍需设备验证。

### 架构与来源

Kirikiroid2 的 `InternalPlugins.cpp` / `PluginImpl.cpp` 使用内置 ncbind 模块，不运行游戏携带的 Windows DLL。本次遵循这一架构，在 `krkrsdl2/src/plugins/alphamovie/` 注册真正的 `AlphaMovie.dll` 模块，由游戏原有的 GFX_AMovie / GenericFlip 或 KAG handler 调用。公共兼容脚本不再定义同名占位类。

解码代码和原生 API 注册形式参考 [krkrsdl3 的 AlphaMovie.cpp](https://github.com/krkrsdl3/krkrsdl3/blob/main/plugins/AlphaMovie.cpp)，所查文件最后修改提交为 `f3b76d2`（2026-08-19）。版权文本保存在同目录 `LICENSE.krkrsdl3`。Kirikiroid2 公开树仅找到 ARM 的 `AlphaMovie_mjpeg.h` IDCT 路径，没有完整插件主体，不能声称本次直接复制了完整 Kirikiroid2 AlphaMovie 插件。

另下载 [原作者插件及手册](https://kaede-software.com/krlm/plugin/alphamovie.zip)，以 [KRKRZ 1.4.0r2](https://github.com/krkrz/krkrz/releases/tag/1.4.0r2) 的 64 位宿主运行原 DLL。对照文件、原宿主和探针均在独立 `.zcode/alphamovie-audit/` 目录；未替换游戏的 DLL、脚本、素材或存档。

### 实现

| 文件 | 职责 |
|---|---|
| `AmvMovie.h` / `AmvMovie.cpp` | 输入流抽象、AJPM / FRAM 校验与索引、按帧读取、zlib alpha、受限缓存、播放和下一段控制 |
| `AmvCodec.cpp` | 自定义 Huffman / 整数 IDCT / YUV420 色彩和 DCT alpha 解码 |
| `AlphaMovie.cpp` | 原生类、脚本方法与属性、Layer 输出、ncbind 注册和会话卸载 |
| `src/core/base/sdl2/PluginImpl.cpp` | 插件可用性探测、静态链接 anchor、Plugins.link 和插件列表 |
| `tests/alphamovie_test.cpp` | 容器、解码、播放、异常输入、像素顺序、负 pitch 检查和真实素材探针 |

通过 `TVPCreateStream` 打开输入，保留 XP3 查找、归档优先级和内容过滤契约。支持 AJPM revision 0：mode 1 的 DCT alpha 和 mode 2 的 zlib alpha。检查文件长度、FRAM 跨度、帧数、宏块尺寸、载荷长度、解压长度及 Huffman 输入边界。整数 IDCT 使用明确的 64 位中间量，范围表线程安全初始化，无未对齐整数写入。

帧缓存最多 32 MiB、8 个条目，按字节预算预读；不全片展开。默认 `preloadSamples=5`，大帧受字节预算限制。当前同步逐帧解码，不另起自行推进显示帧的定时器；游戏保留自身 FPSRate / FPSScale 显示时钟。原包 mode 1 素材可直接处理，不要求用补丁 AMV 覆盖它。

### 原 DLL 对照得到的接口行为

测试目标为 `ltAddAlpha` / `dfAddAlpha` 子图层。记录见 `original-contract.txt`、`original-results.json`、`original-pixel-comparison.json`。

- `showNextImage` 返回零起始的已显示帧索引；`frame` 是解码位置，包含预读帧，二者不能混用。本实现同步预读的位置确定；原 DLL 后台线程的位置会随调度略微变化。
- 默认 `loop=true`、`nextLoop=true`、`preloadSamples=5`。`play` 清理预读并从第 0 帧重新开始；播放后设置 frame，下一次读图显示指定帧。
- `stop` 停止播放、释放解码缓存和待切换资源，保留当前文件元数据及解码位置。`clear` 清理解码缓存并保留文件。`finalize` / native Invalidate / 会话卸载释放文件和原生资源。
- 非循环播放持有末帧，isPlaying 仍为真，直至脚本 stop；不自行循环或抢先停止游戏 handler。
- 下一段在当前影片结束时切换；旧末帧返回时下一段的帧数和 loop 元数据已经可见，符合 KAG 的 next_alpha_movie 调用顺序。
- 空 FRAM 只推进帧号，不改目标图层。测试素材存在连续 20 个空帧；seek 回空帧也保留图层尺寸和位置，不能擅自改成 1×1 透明图层。
- 原 DLL 输出完整编码矩形，如 `(0,906,128,176)`，以及高度 1088 的填充行；screenHeight 仍为 1080。不在插件内提前裁剪，否则 GenericFlip 定位会改变。
- 输出为 BGRA，支持负 pitch。即使目标 face/type 为 AddAlpha，原 DLL 仍直接写入像素，本实现不额外预乘。
- numOfFrame、screenWidth、screenHeight、FPSRate、FPSScale 只读；frame / loop / nextLoop / preloadSamples / left / top 按手册读写。

`assignMovieFrame` 是调用方的脚本方法，调用 `showNextImage` 后执行 `flipAssign` / `flipOffset`；不是原生插件应另加的方法。本次保留原有调用链。

### 两种素材格式

验证同时覆盖原始 AMV 与 [Kirikiroid2 兼容补丁仓库](https://github.com/zeas2/Kirikiroid2_patch) 的转换素材。所检查补丁仅提供资源和配置，不包含 GFX_AMovie、AlphaMovie 插件或同名占位类；解码与播放能力由原生模块提供。

转换素材均为 mode 2、1920×1080、30 fps，共 922 帧；原始素材 6 段使用 mode 1/2，共 960 帧。本次支持并检查两套素材，不改写素材帧数。全片 RGBA 展开需要数百 MiB 至超过 1 GiB，故采用按帧读取和受限缓存。

### 已执行验证

1. MSVC 主机检查通过：两种 alpha 解码、空帧、seek、重播、末帧持有、循环、下一段元数据、stop / close、截断输入、错误跨度 / 熵编码、BGRA 和负 pitch。
2. 原包 6 段和作者补丁 6 段，共 **1882 帧**全部解码成功；缓存峰值不超过 32 MiB。见 original-game-full-scan.json、native-full-scan.json。
3. 9 个抽样帧与 krkrsdl3 独立参考解码逐字节一致。对照原 DLL，mode 2 的 zlib alpha 完全一致；DCT / 颜色有最大 4 个灰度级的 IDCT / 色彩舍入差异，未宣称原 DLL 逐字节复刻或所有素材上无差异。
4. Switch NRO 编译成功。模拟器实际执行 XP3 内的原生集成测试，覆盖可用性、子类有 / 无显式基类构造、两种格式、空帧、矩形和 padding、seek、stop、重播、下一段切换、位置和 finalize。跨返回启动器的两次运行均记录 `AMV_NATIVE_INTEGRATION_PASS`，无重复注册异常。
5. 集成夹具约 2.5 秒后主动关闭窗口并返回启动器，这是夹具逻辑；实际游戏验收来自后续用户操作。

用户随后确认实际剧情运行、快速读档与设置界面正常。上述检查不能替代物理 Switch 的画面与性能验收。

### 历史包身份：2026-09-26

- 构建产物：build-switch/krkrsdl2.nro，28,370,933 bytes。
- 本次发布 SHA-256：`3141223231D895D1CF454BE9722A9A147B76A3D1D9F2B5018A23583BCEF7047C`；构建与部署副本一致，包含 P93 的内置插件探测与会话清理修复。
- 原生 AMV 集成证据：krkrsdl2_debug_1790429546.log、krkrsdl2_debug_1790429563.log；本轮日志与部署校验保存在本地审计目录，不随发布包提供。

先前的快速读档归档优先级修复和实际队列音频清理保留，未修改游戏脚本或存档 ID。

<a id="storage-move"></a>

## 原生文件移动与快速存档轮换

部分脚本在已有快速存档时，会先调用 `Storages.moveFile(from, to)` 将旧存档
轮换到下一槽位。此前内置 `fstat.dll` 已能成功加载，但只提供了部分存储接口，
未注册 `moveFile`。因此第一次保存可以成功，再次保存触发轮换时会抛出
`Member "moveFile" does not exist`，随后返回启动器。

### 实现

`StorageIntf.cpp` 注册原生静态 `Storages.moveFile`，两个参数不足时返回
TJS 的参数数量错误；正常调用返回是否成功移动。

- 沿用存储层路径解析：源文件允许自动搜索路径，目标使用精确的规范化路径。
- 两端必须是可访问的本地文件路径；不允许移动 XP3 成员或写入归档。
- 使用 UTF-8 本地路径与 Switch fsdev 的 `rename`，接口层处理 Unicode 与设备路径规范化。该接口的模拟器测试不代表中文游戏目录在真机可用；**Switch 真机的游戏路径不能包含中文。**
- 按参考插件公开契约拒绝已存在的目标与同名移动；移动失败保留原文件。
- 使用存储介质的无 `stat()` 存在性检查；成功后清理存储缓存。
- 即使调用者丢弃返回值，仍执行移动操作。

接口契约参考 `krkrsdl3/plugins/fstat.cpp` 的 `moveFile`。平台实现采用原生重命名，
避免参考实现中复制失败后仍删除源文件的风险。不创建目标父目录。

### 验证

```powershell
python tests/build_storage_move_fixture.py --nro build-switch/krkrsdl2.nro --output-dir build-storage-move-fixture
```

脚本生成独立 NRO 副本，保留可执行代码，仅替换 RomFS；不修改正式 NRO，
测试写入仅限 `sdmc:/switch/KRKR-ns/storage-move-test/`。

2026-09-26 在模拟器运行完整 Switch ARM 引擎，日志出现
`[storage-move] COMPLETE checks=36`：中文路径、双斜杠设备路径、缓存更新、
内容保留、目标冲突、源文件不存在、父目录不存在、同名与空参数、归档拒绝、
丢弃返回值，以及连续四次快速存档轮换全部通过。

原先报错的运行流程连续快速存档两次成功，旧文件轮换至第三槽位后的 SHA-256
与修复前备份一致；用户确认本次部署的 NRO 正常。此记录不代替 Switch 实机验证。
