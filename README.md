<div align="center">

# KRKR-ns

**吉里吉里（KiriKiri）视觉小说引擎 · Nintendo Switch 移植版**

基于 [krkrsdl2](https://github.com/krkrsdl2/krkrsdl2)（pinned `bf207f2`）· 内嵌 krkrsdl2/krkrz（pinned `b11c43a`）

[![Release](https://img.shields.io/github/v/release/DrFlyingPig/krkr-ns?style=flat-square)](https://github.com/DrFlyingPig/krkr-ns/releases/latest)
[![License](https://img.shields.io/badge/license-MIT-green?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-red?style=flat-square)]()

**[⬇️ 下载最新版](https://github.com/DrFlyingPig/krkr-ns/releases/latest)**

</div>

---

## 📖 说明介绍

KRKR-ns 将 PC 端吉里吉里（KiriKiri / KRKR）引擎完整移植到 Nintendo Switch，可直接运行未加密 `.xp3` 打包的 KRKR / KAG 视觉小说，也支持展开的目录式游戏。

内置游戏浏览器启动器：扫描 `sdmc:/krkr/` 下的游戏目录，点选即玩。游戏内选择「结束游戏」会直接**返回启动器**，可以立刻更换下一款游戏，无需退出程序——真机通过系统链式重启获得全新引擎，模拟器则以同进程整引擎重建达到同样效果。

## ⬇️ 获取与使用

1. 从 [Releases](https://github.com/DrFlyingPig/krkr-ns/releases/latest) 下载 `vX.Y.Z.nro`，放到 SD 卡的 `sdmc:/switch/` 目录下。
2. **真机**：进入 HBMenu 加载（相册方式需按住 `R` 进入 title-override）；**模拟器**：直接加载该 nro（Ryujinx 系 fork 均可）。
3. 游戏放到 `sdmc:/krkr/<游戏目录>/`，启动器会列出其中的 `.xp3`，选择即可开始；同目录的其余 xp3 会自动挂载为资源包。
4. 存档按游戏隔离存放于 `sdmc:/switch/krkrsdl2/saves/<游戏目录>/`。

> 诊断日志位于 `sdmc:/krkrsdl2_debug.log`；运行时开关（标记文件）与全部源码级补丁清单见 [PATCHES.md](PATCHES.md)。
> 自行构建：devkitPro 工具链（`NINTENDO_SWITCH=ON`），`build_nro.sh` 一键完成构建、打包与模拟器部署。

## ✅ 已实现功能

**启动器与多游戏管理**

- 游戏浏览器：扫描 `sdmc:/krkr/`、点选 xp3 直接启动、同目录资源包自动挂载
- 游戏内「结束游戏」→ 返回内置启动器，连续更换游戏（真机链式重启 / 模拟器同进程重建）
- 存档按游戏目录隔离；compat 补丁目录（SD 卡）可覆盖内置适配层

**引擎兼容与适配**

- KAG2 / KAG3 兼容层（Switch 桩替换桌面版插件脚本，自动维持优先级）
- 常用插件内置化：E-mote（emoteplayer / motionplayer）、psbfile、textrender、kagparser、csvparser、layerExBTOA 等
- 缺失资源垫图、`Layer.loadImages` 容错、脚本异常记录并继续（风暴熔断）

**图形与性能**

- E-mote 立绘动画：隔离 GL 后端 + 驱动自检（异常驱动自动回退 CPU）、脏区回读、半分辨率可选
- simde SIMD 混合内核（NEON）、4 核绘制线程池
- GPU 呈现链直通（FBO blit）、XP3 段缓存、增量资源索引、解码图像缓存
- 手柄 → 鼠标/键盘事件合成（NS 物理键位语义：B 确认 / A 取消等）

**诊断基建**

- 统一 SD 日志、每帧分段剖析（`[prof]`）、心跳与阶段标记、资源未解析探针（`[miss]`）、帧捕获与图层树转储

**暂未实现**：视频播放（内置占位，计划接入 FFmpeg 管线）。

## 📄 许可

本仓库以 [krkrsdl2 的 MIT 许可](LICENSE) 发布；内嵌/引用的上游组件（krkrz 引擎、FAudio、SDL2 及其 Switch 端口、simde、zlib、FreeType、libjpeg-turbo、libpng、libogg/libvorbis、libopus 等）各自保留其原始许可证与归属，见各组件目录内的 LICENSE/COPYING 文件。

**本项目不包含、也不分发任何商业游戏资源。**
