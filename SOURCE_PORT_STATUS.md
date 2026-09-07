# 完整 KRKR 源码移植状态

2026-09-05。目标：通用 Nintendo Switch 移植，游戏只作为回归样本。此文件优先于早期规划中未经验证的“支持”标记。

## 固定的参考源码

| 项目 / commit | 用途 | 本地位置 |
|---|---|---|
| krkrz/krkrz `fd5c4baa6a2ef5978db1bd043634351f48667daf` | 原引擎 Layer/DrawDevice、位图、脚本语义 | `.zcode/upstream-krkrz` |
| krkrz/krkrz_dev `472482fc0bd2f57d95d260e6970e847b3e8cf38d` | 完整发行工程及标准插件/测试清单 | `.zcode/upstream-krkrz-dev` |
| zeas2/Kirikiroid2 `d1c2b1259423542c893e0b65eaeb46c848848f2b` | 完整跨平台运行时、静态插件加载语义 | `.zcode/upstream-kirikiroid2` |
| 2468785842/krkr2 `dca7264572a63e753c1b07ca7053513d1751ce70` | 现代完整跨平台项目补充对照 | `.zcode/upstream-krkr2` |
| krkrsdl2/krkrsdl2 `bf207f27d683834d35146736ec8d83d397ec75a2` | 当前平台原型 | `krkrsdl2`（含已有本地修改） |
| krkrsdl2/krkrz `b11c43a07543756fcb943a60ff0a949a7f470f10` | 当前内嵌引擎 | `krkrsdl2/external/krkrz`（含已有本地修改） |
| krkrsdl3/krkrsdl3 `5a8bd422f82d3758045f403520a64b772a59f40c` | E-mote 开源渲染实现，仅作此子系统参照 | 临时参考 checkout，不作为完整工程验收依据 |

原版标准测试 `krkrz/test_scripts` 固定 `8ca5cf18b7d5a70d0af12da713fe8de029e7cb95`；KAGParser 固定 `b26f47607b65ef8e69a88ff085709500f81b3224`。引用/移植源文件必须保留许可证及归属；这些工程的许可证不一，不能把整个派生项目统一宣称为 MIT。

## 本轮落实的通用修正

- DIB → SDL：正确处理上下向行序、源/目标偏移、裁剪、行距；局部 `SDL_UpdateTexture` 从脏矩形的首像素传入，而非整幅图像原点；上传成功后清理待更新区域。
- E-mote：按固定上游 GL 后端实现独立 GL 上下文、FBO、二值蒙版、分模式 RGB/alpha 混合。与窗口 SDL 渲染器隔离并恢复原上下文，避免污染窗口纹理/裁剪状态。CPU 回退相应校正混合与共享三角形边界；CPU 的最近邻采样仍不等价于 GPU 的线性/多级采样。
- 插件：注册表只登记实际静态模块。未知 DLL 的兼容性忽略不等于成功实现；`Plugins.getList()` 不再列出未实现 DLL。无法卸载的静态模块保留注册状态。
- BasicDrawDevice 多管理器：完整源码有单管理器约束，旧移植擅自取消约束并将所有管理器直接写入同一表面。正在验证生命周期与离屏图层，不将这个扩展视为正确实现。

## 不得标记为完成的范围

| 子系统 | 当前边界 / 下一步 |
|---|---|
| TJS2 / KAGParser | 引擎在编译执行，但标准上游测试尚未完整跑过；必须验证 bytecode、异常、计时器、脚本原生类接口 |
| Layer / 绘制与转场 | 位图边界测试通过不代表复杂场景正确；用户报告的裁切/旧场景矩形仍需同场景视觉复验 |
| E-mote | 有开源后端且可绘制，不是官方 E-mote 全功能；原有 TODO（风、timeline 等）仍存在 |
| LayerExImage / LayerExDraw / LayerExRaster / scriptsEx | 未完成源码移植，不能以 DLL 加载无异常作为支持证据 |
| TextRender / PSBFile | 已有移植实现，但并不自动等于上游完整接口兼容 |
| windowEx / Menu / Pad / k2compat | 仍有大量 TJS 空实现；需要逐项平台实现或明确报告不可用 |
| 视频 / AlphaMovie | Switch 视频路径未完成；不能把跳过视频视为已支持 |
| 音频 | 仍有真实数据的 open 异常，需要分别核对资源解析、解码器和播放时序 |
| Switch 真机 | 本轮仅 Windows 主机测试及 Switch NRO 模拟器运行；未进行真机验收 |

## 可重复测试

主机：`cmake -S tests -B build-tests`（指定 SDL2 和 MSVC 生成器），`cmake --build build-tests --config Release`，`ctest --test-dir build-tests -C Release --output-on-failure`。

- `bitmap_bridge_test`：DIB 行序、偏移、行距、非法区域、局部纹理更新。
- `emote_gl_test` / `--cpu`：各混合模式、mask 阈值、共享边、负行距及 SDL 状态隔离。
- `tests/fixtures/core_port/startup.tjs`：真实 TJS 引擎的独立 640×360 场景，插件注册、完整画面、移动子图层与右下角局部更新。测试 NRO 不包含游戏或存档。

模拟器专用 GCS unwind 补丁和真机正常构建需要分开交付；模拟器通过不能证明真机通过。保留 `.zcode/port-source-audit-baseline/krkrsdl2.nro`，不覆盖此前基线。
