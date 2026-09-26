# 原生文件移动与快速存档轮换

部分脚本在已有快速存档时，会先调用 `Storages.moveFile(from, to)` 将旧存档
轮换到下一槽位。此前内置 `fstat.dll` 已能成功加载，但只提供了部分存储接口，
未注册 `moveFile`。因此第一次保存可以成功，再次保存触发轮换时会抛出
`Member "moveFile" does not exist`，随后返回启动器。

## 实现

`StorageIntf.cpp` 注册原生静态 `Storages.moveFile`，两个参数不足时返回
TJS 的参数数量错误；正常调用返回是否成功移动。

- 沿用存储层路径解析：源文件允许自动搜索路径，目标使用精确的规范化路径。
- 两端必须是可访问的本地文件路径；不允许移动 XP3 成员或写入归档。
- 使用 UTF-8 本地路径与 Switch fsdev 的 `rename`，支持中文路径和设备路径规范化。
- 按参考插件公开契约拒绝已存在的目标与同名移动；移动失败保留原文件。
- 使用存储介质的无 `stat()` 存在性检查；成功后清理存储缓存。
- 即使调用者丢弃返回值，仍执行移动操作。

接口契约参考 `krkrsdl3/plugins/fstat.cpp` 的 `moveFile`。平台实现采用原生重命名，
避免参考实现中复制失败后仍删除源文件的风险。不创建目标父目录。

## 验证

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
