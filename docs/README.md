# KRKR-ns 文档索引

> 根目录只保留 [README.md](../README.md)（面向使用者）；其余文档都收录在这里。

## 兼容性与移植记录

| 文档 | 内容 |
|---|---|
| [PATCHES.md](PATCHES.md) | 全部源码层补丁记录（P1 起，含背景与验证方式） |
| [COMPAT_BACKLOG.md](COMPAT_BACKLOG.md) | 兼容性缺口清单（对照 krkrsdl3，P0–P3 优先级与参考实现） |
| [UPSTREAM_DELTA.md](UPSTREAM_DELTA.md) | 与两个上游基线的差异清单（`tools/upstream_delta.sh` 自动生成，勿手改） |

## 规划与开发记录（本地保留，不入库）

按 `.gitignore` 约定保留在本地、不提交：

| 文档 | 内容 |
|---|---|
| [PORTING_PLAN.md](PORTING_PLAN.md) | 移植规划（历史文档，含 2026-09-05 范围纠正） |
| [SOURCE_PORT_STATUS.md](SOURCE_PORT_STATUS.md) | 源码基线、验收边界与固定参考 |
| [DEVLOG.md](DEVLOG.md) | 开发记录（2026-09-03 起） |
| [OPTIMIZATION_PLAN.md](OPTIMIZATION_PLAN.md) | 性能优化方案与阶段划分 |
| [MENU_PERFORMANCE.md](MENU_PERFORMANCE.md) | 菜单公共路径优化与验证（P62） |
| [FREEZE_DIAGNOSIS.md](FREEZE_DIAGNOSIS.md) | 长剧情卡死 / 字体生命周期 / 选项居中定位（P63） |
