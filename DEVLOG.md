# KRKR-ns 开发复盘(2026-09-03 ~ 09-04 会话)

本文档整理本次移植窗口的完整记录:做了什么、实现了哪些需求、开发流程、以及最重要的踩坑清单。配套文档:
- `PORTING_PLAN.md` — 已批准的移植计划(阶段/里程碑/风险)
- `PATCHES.md` — 全部代码层补丁的技术清单(本文踩坑处均有对应补丁条目)

---

## 一、这个窗口都干了什么

### 1. 里程碑收尾
- **M1 正式收官**:显示修复组合拳(构造器纹理创建时机 + 图层显式尺寸 + 逻辑分辨率)在模拟器与真机**双端全屏红确认**。
- **Phase 3 输入完成**:手柄→鼠标/键盘事件合成,模拟器日志与真机按键双重验证(A=蓝右击、B=绿左击,NS 物理键语义)。

### 2. 键位语义修正(用户裁定)
- NS 物理键位与 Xbox 位置语义相反(A 右下、B 下、X 右上、Y 左上)。
- `SDL_CONTROLLER_BUTTON_*` 按 Xbox 位置语义处理,合成动作与 NS 物理键对齐(交换 A/B、X/Y),真机验证通过。

### 3. 产物命名统一(用户要求)
- 构建产物固定为 `build-switch/krkrsdl2.nro`,不再出现 `-RED3/-INPUT7/-patched` 等后缀;模拟器入口固定 `portable/games/krkrsdl2.nro`。

### 4. 多图层支持(Phase 6a)
- 放开 `tTVPBasicDrawDevice` 的单个 LayerManager 限制(上游历史遗留);
- 确认 KRKR 规则:**顶层图层不可移动**("Cannot move primary layer"),**子图层可移动**——KAG 式布局成立。

### 5. 真实游戏加载链(Phase 6b,核心工作量)
用户提供《星光咖啡馆与死神之蝶》(不拷贝、junction 路径映射),模拟器实测打通:
- 11 个 xp3 自动挂载(含中文名,UTF-8 转换)
- 四种存储路径形态全部本地化(`?/`、`file://?/`、`file://sdmc:`、前导 `/`)
- 游戏 startup.tjs(字节码)→ system/Initialize.tjs → KAG 初始化 → 47740 文件索引
- 插件兼容体系:link no-op + 假注册表 + **补丁目录覆盖机制**(sdmc 文本文件覆盖 xp3 内同名脚本,改文件即迭代、无需重建)
- 已连续翻过:win32dialog / k2compat 全家 / conductor / affinesourcemotion(Motion),当前推进到 menus.tjs

### 6. ONScripter NS 版调研(用户建议)
提炼成熟方案:手柄映射表(A=Enter/B=Esc/X=跳过/Y=自动)、字体三层兜底(游戏目录→romfs 内嵌→SD 卡)、每游戏独立存档目录、多游戏浏览器、hid 直轮询触摸,列入 Phase 6c 执行清单。

---

## 二、实现了哪些需求(对照计划)

| 里程碑/阶段 | 内容 | 状态 |
|---|---|---|
| M1 | 引擎启动 + 画面显示 | ✅ 双端全屏红确认 |
| M2 | 手柄输入可用 | ✅ 真机确认(NS 物理键语义) |
| Phase 3 | 输入与交互(光标/点击/键盘合成) | ✅ 完成 |
| Phase 6a | 多图层合绘 | ✅ 放开限制+子图层验证 |
| Phase 6b | 真实游戏加载 | 🟡 启动链推进至 KAG 菜单初始化,插件兼容补丁持续迭代中 |
| Phase 4 | 存档路径 | ⏳ 未开始(已定方案:独立 save 目录) |
| Phase 5 | 字体/音频/性能 | ⏳ 未开始(字体兜底方案已定) |
| Phase 7 | 打包发布 | ⏳ 未开始 |

---

## 三、开发流程(每个问题怎么被解决的)

**核心循环(分钟级)**:
```
改代码 → bash build_nro.sh → 复制 NRO 到模拟器 games/ → 重启模拟器
→ grep 日志(sdmc:/krkrsdl2_debug.log)→ 定位崩溃点 → 修复 → 复测
```
补丁目录阶段后循环更短:**改 sdcard/switch/krkrsdl2/patch/system/*.tjs → 重启模拟器**(无需重建)。

**调试手段栈(按使用频率)**:
1. **fd 日志**(`krkrsdl2_debug.log`):所有启动/存储/输入关键路径打点;`Debug.message`(TJS 侧)在 Switch 上镜像到同一日志——脚本层也能打日志。
2. **异常自动反汇编**:TJS 异常时引擎自动 dump VM 寄存器+最近指令(TJS 内置)——`gpd %1, %-2.*1 "WIN32Dialog"` 这类输出直接揭示崩溃点语义。
3. **hex dump 打印**:`AsStdString` 在 Switch 上输出不可读(见坑 5),调试时用 UTF-16 单元 hex 打印。
4. **xp3 提取器**(临时 Python 工具):读 zlib 索引+segment,提取游戏脚本/字节码做字符串分析("FOUND 名"/方法名 dump)。
5. **真机验证**:由用户拷贝 NRO、反馈现象、MTP 只读拉日志;模拟器可复现的问题优先模拟器解决,键位等环境差异以真机为准。

**验证策略**:模拟器(Nextendo)先行、真机兜底;两环境行为差异(如 stat、chdir、虚拟手柄)都要记录并区分。

---

## 四、踩的坑(最重要,按类别)

### A. 环境/构建类
1. **模拟器加载到旧文件(Games 目录递归扫描)**
   现象:日志出现已删除构建的标记(`BUILD=TEXFIX2-2140`)。
   真相:Nextendo **递归**扫描 `portable/games/`(含 `_old/` 备份子目录),加载了旧 NRO。
   解决:games 目录只留单一入口;备份移到模拟器目录之外;构建标记+md5 双核对。

2. **`mklink /J` 中文目标路径被拼错盘符**(cmd 编码/转义地狱)
   现象:目标变成 `D:\C:\Users\...`。
   解决:改用 PowerShell `New-Item -ItemType Junction`(原生 Unicode,一次成功)。

3. **devkitPro 全局变量过期**(`/opt/devkitpro`)——脚本内强制 `D:/devkitPro`。

### B. 存储/路径类
4. **`chdir()` 在模拟器 fs 层无效**
   现象:相对名解析始终命中 romfs 的 startup.tjs。
   真相:KRKR 相对名规约用**引擎自己的** CurrentDirectory(`TVPSetCurrentDirectory`),与 POSIX cwd 无关。
   解决:游戏模式显式 `TVPSetCurrentDirectory(game_dir)`。

5. **`ExePath()` / `TVPGetAppPath()` 是 `static` 缓存**
   现象:游戏模式切换后 `System.exepath` 仍是 romfs(插件探测路径全错)。
   真相:首次调用(挂载前)=romfs,永久缓存。
   解决:去掉 static,每次动态计算。

6. **fsdev 路径的四种形态都要处理**
   - `?/<dev>`(媒体剥离后)、`file://?/<dev>`(P5 原补丁)、`file://sdmc:/...`(ExePath 派生)、**前导 `/`**(`/sdmc:/...`,规约链产生,fsdev 打不开)。
   曾误判 junction 子目录问题——真凶是前导斜杠(见日志 `exist fopen-FAIL: /sdmc:...`)。

7. **模拟器 `stat()` 挂起/失败**
   现象:存在性检查(插件 dll 等)恒 false。
   解决:`CheckExistentStorage` 在 __SWITCH__ 用 `fopen` 探测替代 `stat`。

8. **中文文件名 Fatal**
   现象:`ttstr(窄字节)` 转码遇到 `启动游戏.xp3` 直接 "Cannot convert given narrow string to wide string" 退出。
   解决:显式 `TVPUtf8ToUtf16` + 单个失败跳过。

### C. 引擎/脚本语义类
9. **`AsStdString()` 返回 `char16_t` 串**(krkrz 定制,非 std::string)
   现象:所有打印变 "?";与 `const char*` 三元混用编译错。
   解决:压测打印用 hex/长度;拼日志避免 char16_t 与 char* 混合。

10. **TJS 细节三连**
    - `int` 没有 `toString(16)`(脚本 `key.toString(16)` 崩)→ 十进制拼接。
    - `tTVPMouseButton` 枚举:`mbLeft=0, mbRight=1, mbMiddle=2`(与 Windows 1/2/4 不同)→ 测试脚本曾用 `==2` 判右键导致 A/B 同色。
    - **`{}` 对象字面量(带成员)在文本模式 Syntax error;`new Object()` 无 `Object` 全局** → 一切"命名空间对象"用函数对象(`function NS(){}` + 挂成员)。

11. **`class X extends 插件类` 在插件缺失时崩溃**
    现象:`WIN32DialogEX extends WIN32Dialog` → "Member WIN32Dialog does not exist"。
    分析链:脚本内 `var WIN32Dialog = undefined` 遮蔽全局 stub → TJS 对 void 值成员报 Member not exist。
    解决:补丁目录同名覆盖脚本(文本版),补丁内定义可 extends 的 TJS class。

12. **KAG 的 `Plugins.isAvailable` 决定脚本加载分支**
    现象:isAvailable=false 时 `system/motion.tjs` 不被加载 → `Motion` 类缺失连锁崩溃。
    解决:假注册表(link 记录、getList 报告已加载)让 isAvailable=true,配合补丁类 stub。

13. **字节码脚本(TJS2100)无法文本替换** → 补丁目录覆盖(文本版优先于 xp3 字节码)。

### D. 渲染/输入类
14. **BasicDrawDevice 单 LayerManager 限制**(D3D 时代遗留)
    现象:第二个顶层 Layer 直接 Internal Error。
    解决:放开检查(合成路径本身支持多管理器)+ 顶层不可移动改子图层。

15. **模拟器虚拟手柄不可靠**
    - 键盘映射与 Config.json 不符(z→B、c→Y);8 个手柄按钮位漂移(同键时报 B 时报 DpadRight)。
    - 结论:模拟器仅验证"链路通断",键位语义以真机为准。

### E. 工具/流程类
16. **Edit 工具与外部修改冲突**("File modified since read" 反复);`#if 0` 块平衡被误破坏 → 小步编辑或 python 原子替换。
17. **bash heredoc 中文/转义地狱**(`\U`、`\\n`、mklink 引号)→ 单引号 heredoc、PowerShell、避免在 bash 内联写 Windows 路径。

---

## 五、现状与下一步

**当前状态**:游戏初始化推进到 `menus.tjs`(MenuItem 类缺失,下一个补丁点)。补丁循环已验证高效(每轮约 3 分钟),按此迭代可抵达标题画面。

**待办(按优先级)**:
1. 继续补丁循环至标题(menus → 后续符号)
2. 真机版验证(插件目录真实可读,行为可能与模拟器不同)
3. Phase 6c:手柄映射按 ONScripter 表优化、字体兜底(romfs 内嵌 font.ttf)、存档独立目录、多游戏浏览器
4. Phase 5:FAudio 音频验证、性能基准
5. Phase 7:打包发布(图标/NAC P 信息/README)

**既定约定**:
- 游戏目录:`sdmc:/switch/krkrsdl2/game/`(xp3 或解包目录)
- 补丁目录:`sdmc:/switch/krkrsdl2/patch/system/`(同名覆盖)
- 日志:`sdmc:/krkrsdl2_debug.log`
- 交付产物:`build-switch/krkrsdl2.nro`(唯一命名)