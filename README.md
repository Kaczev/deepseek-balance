# deepseek-balance

Windows 桌面挂件：无边框、圆角、每像素半透明的浮动面板，实时显示 DeepSeek 账户余额，
带余额变化曲线、心跳与氛围光。单个静态链接的 exe，不需要运行库。

**用户向的说明（怎么跑、怎么给密钥、怎么关）在 `dist\README.md`。这份文件是开发说明。**

---

## 构建

前提（本机已验证的组合）：

- Visual Studio **Build Tools**（不是完整 IDE 也行），带 C++ 工具集
- Windows SDK
- CMake 与 Ninja —— 用 Build Tools 自带的即可，不必单独装

```bat
build.bat
```

产物是 `build\dshb.exe`。脚本自己在 `build\build.log` 里写日志，不往 stdout 写。

**`build.bat` 的规则**

- `vcvars64.bat` 必须先跑；`cl` / `cmake` / `ninja` 都不在 PATH 上。
- 跑完 `vcvars64.bat` 之后**绝不再碰 PATH**。
- **任何源文件或头文件比 `build\dshb.exe` 新，就把对象缓存整个丢掉重编。**
  这条不是保险起见，是必须的：本项目生成的 `build.ninja` **没有 MSVC 头文件依赖跟踪**
  （没有 `deps = msvc`，也没有 depfile），ninja 只看 `.cpp` 的时间戳。
  所以改 `src\tuning.h`（所有者唯一会调的文件）会打印 `ninja: no work to do`，
  旧 exe 原地不动，挂件继续显示旧尺寸 —— 实测过。
- `*.rc` 与 `*.png` 也算在内：图标 PNG 是被 `.rc` 当普通文件引用的，
  资源过期不会有任何报错，托盘就一直显示旧图。

**这个文件是纯 ASCII 的，请保持。** cmd.exe 按 OEM 代码页解析 `.bat`，
UTF-8 中文注释会被撕成一条条野生命令。

---

## 跑

```bat
build\dshb.exe
```

没有安装步骤，没有注册表写入。

`--selftest --seconds=N` 跑 N 秒后自动退出，并把窗口与 DPI 的事实、帧统计写进
**exe 旁边的 `selftest.log`**（路径来自 `GetModuleFileNameW`，所以是 exe 所在目录，
不是当前工作目录）。程序是 GUI 子系统，**没有控制台，写 stdout 等于丢掉** —— 所以日志只落文件。

日常观察用的脚本（都不写数据目录）：

| 脚本 | 做什么 |
| --- | --- |
| `run.bat` | 正常启动一次 |
| `run-long.bat` | 跑长一点，用来看滚动动画 |
| `run-roll.bat` | 反复跳变，用肉眼反复看余额滚动 |
| `run-fixed.bat` | 把余额钉在一个固定值上不动 |

---

## 数据目录

```
%LOCALAPPDATA%\deepseek-balance\
```

路径由 `src\paths.cpp` 的 `Paths()` 决定（`SHGetKnownFolderPath(FOLDERID_LocalAppData)`；
取不到时兜底到 exe 旁边）。启动时会**真的建目录 + 写一个临时文件再删掉**才敢说可写 ——
只看目录是否存在是骗人的。不可写时往 `%TEMP%\deepseek-balance-unwritable.txt` 记一条原因。

| 文件 | 谁写的 | 说明 |
| --- | --- | --- |
| `config.json` | `src\panel_drag.cpp` | 面板位置，拖动/吸附后更新 |
| `curve.json` | `src\curve_store.cpp` | 曲线点历史；**只在余额变化时记点** |
| `samples.jsonl` | **没有代码在写** | `Paths()` 里声明了它，只有 `--selftest` 的日志会打印这个路径 |
| `widget.log` | **没有代码在写** | 同上；日志实际写在 exe 旁边的 `selftest.log` |
| `fx.json`、`wheel-trace.log` | **没有代码在写** | 已删功能留下的旧文件，不会被读 |

后三行是历史遗留，不是设计。看到它们不要以为有功能在用。

**测试时永远不要碰所有者自己的 `config.json` / `curve.json`。**

```bat
build\dshb.exe --curve-store=C:\tmp\curve.json --config=C:\tmp\config.json
```

这两个参数就是为了这个而存在的测试旁路，量"位置持久化"和"曲线写入"的探针必须能反复写文件，
而绝不能写所有者那一份。

---

## 密钥

两个来源，**按顺序**（`src\main.cpp` 的 `wWinMain` 启动段）：

1. 环境变量 `DEEPSEEK_API_KEY`
2. 没有则读 `%USERPROFILE%\.dsh\.credentials.yaml` 里 `refs.DEEPSEEK_API_KEY` 那一行

只做最小解析（逐行找那一行），所以不引入 YAML 依赖。**值从不打印、从不写日志**，
只记"用了哪个来源"。两个来源都没有时进 `NoKey` 态，不发任何请求，
标题行显示 `未找到 DEEPSEEK_API_KEY`（`src\widget_display.cpp`）。

这个顺序还有一个理由：真接口必须在主循环**喂第一帧数据之前**就决定好要不要用。
否则假数据源会先喂一个假值，真请求一慢或一失败，那个假值就一直挂在屏幕上（实测卡在 99.80）。

---

## 关闭流程

`src\widget_display.cpp` 里的关闭状态机，三个自由函数 `ShutdownEnter` / `ShutdownClick` /
`ShutdownCancel`。**0.2 删掉了全局 Esc 钩子**：那个钩子会让任何程序里按 `Esc` 都关掉挂件。

- 右键（非关闭态）→ 进入。进入要求宿主已经装好全局鼠标钩子（唯一那条取消路要用它），
  装不上就进不去 —— 进去出不来比进不去更糟。
- 关闭态里左键和右键**都算一次**，所以 `main` 必须按"当前是否关闭态"分派右键的两种含义。
- **三次之间没有时间窗**：一个时间戳都不存。
- 第三条点击（或托盘菜单的「关闭」）发出"该放粒子了"的信号，此后不再计数、不再受理输入。
- 取消只有一条路：点到面板实体之外。

`ShutdownFireNow()` 是托盘菜单那一项的实现：把 clicks 补齐到 3 再走第三次点击那一步，
终点状态与在面板上点满三下逐位相同。它**不是**"连调三次 `ShutdownClick`"的马甲。
托盘右键菜单里只剩「关闭」这一项（所有者定的）。

拖动与边缘吸附在 `src\panel_drag.cpp`：钳制规则是"面板**面积**的 ≥50% 落在某个显示器的工作区内"，
不是"面板左上角在屏内"。吸附发生在拖动结束时。位置存进 `config.json`。

---

## 进度表

已做、未做、以及每一步的验收标准写在 `不入库文件\` 下的步骤文档里（该目录不入库）。
做设计决定之前先看那里，再改代码。

---

## 调试开关

全部在 `src\main.cpp` 的 `wWinMain` 参数解析里。挑常用的：

**看数字与状态**

| 开关 | 作用 |
| --- | --- |
| `--selftest [--seconds=N]` | 跑 N 秒，把窗口/DPI/帧统计写进 exe 旁的 `selftest.log` |
| `--selftest-b` | 金额解析与状态机的自检（不建窗口也能看的那部分） |
| `--debug` | 显示调试浮层 |
| `--fixed-amount=N` | 把余额钉在 N，不再取样、不再变化 |
| `--real=N` / `--last=N` | 手动设定实际采样值 / 上次的值 |
| `--seq=a,b,c` / `--step=` | 喂一串采样值，看滚动与曲线怎么走 |
| `--no-anim` | 显示数字不做指数平滑，直接到位 |
| `--phase=P` | 停在行程的哪一刻（0..1） |
| `--scenario=N` | 假数据源的场景，收 1 起的序号或名字（`steady` / `fast` / `low` / `recharge` / `zero` / `unavailable` / `nonetwork` / `stale` / `clockjump`）；**名字不认识会明说，不再被悄悄当成 0** |
| `--speed=X` | 假数据源的时间倍速 |

**导帧**（帧状态是纯函数，一帧一个进程；这套惯例值得照抄）

| 开关 | 作用 |
| --- | --- |
| `--export-frame=k --out=路径.png` | 用同一份绘制代码离屏渲染第 k 帧到 PNG |
| `--curve-frame=k` | 把曲线滚动计时器冻在第 k/60 秒 |
| `--beat-frame=k` | 把心跳仿真时刻放到第 k/60 秒 |
| `--shutdown-frame=k --shutdown-clicks=N` | 把关闭态钉在"已点 N 下、进入以来第 k 帧" |
| `--countdown=N` | 导帧不给倒计时真实来源，用它钉一个值 |

**真实路径的运行时夹具**（会动真窗口，只在显式传参时做）

| 开关 | 作用 |
| --- | --- |
| `--shutdown-test=T` | 三击剧本：第 2 击后停 T 秒再点第 3 击（跑 1 与 12，后者就是"没有时间窗"的证据） |
| `--shutdown-enter-at=T` | 三击剧本从第 T 秒开始（R 只抬不降，冷启动 R=0，所以有时要等它衰减） |
| `--shutdown-hold` | 进关闭态后挂住，等外部 `SendInput` 一次真实点击 |
| `--tray-probe=1` | 跑完把托盘的事实逐条写进日志：图标矩形、菜单项数与文字、窗口句柄与 uID。**不弹菜单** |
| `--tray-menu-test=N` | 第 N 秒弹**真**菜单并真的点「关闭」。`=5` 只弹不点，`=7` 连点 5 次右键。本次**不挂真图标** |
| `--no-tray` | 整轮不碰通知区域，因此绝不会留下僵尸图标 |
| `--no-mouse-hook` | 当作全局鼠标钩子装不上（测试旁路：右键进不了关闭态） |
| `--frame-stats=N` | 记满 N 帧的 p50/p99 后退出 |
| `--no-present` | 画完不提交，量光栅代价（不含等垂直空白） |
| `--dpi=N` / `--ui-scale=S` | 覆盖画布 DPI / 视觉缩放（1.0 = 按 DPI，设计意图） |
| `--force-new-instance` | 跳过单实例检查（单实例靠 `Global\deepseek-balance-v0.2` 互斥体） |

**氛围与面板的 A/B 尺子**

| 开关 | 作用 |
| --- | --- |
| `--no-text` | 正文一层都不画（量底色对比度时必须先去掉文字） |
| `--no-curve` | 关掉氛围曲线，确认"关掉后文字位置逐像素不变" |
| `--pause-ambience` | 暂停期间氛围冻结，用来证明"暂停中连导两帧逐位相同" |
| `--ambience=R,D` | 把这一帧的氛围钉在给定的 R、D |
| `--crisp` / `--digit-draw=MODE` | 数字的绘制口径（`static` / `axis` / `user` / `rest`） |
| `--roll=N` / `--roll=loop` / `--roll-to=X` | 滚动抓帧 / 肉眼循环观察 |
| `--particles-only` / `--no-particles` / `--shutdown-particles` | 粒子层的 A/B：只画粒子、一颗粒子都不撒、在真实循环里触发 |

带 `--api-` 前缀的一批（`--api=off`、`--api-once`、`--api-host=`、`--api-port=`、
`--api-plain-http`、`--api-timeout-ms=`、`--api-interval-ms=`）用于把真接口指到别处或关掉。

所有可调常量**只有一个来源**：`src\tuning.h`。曾经把 `10000` 硬编码在 `main.cpp` 里，
改常量完全无效 —— 别再那样做。改 `tuning.h` 必须重新编译（见上面 `build.bat` 那条规则）。

---

## 探针

**离线探针**（不弹窗口、不碰网络、不写所有者数据）：

| 目标 | 量什么 |
| --- | --- |
| `closeprobe` | 关闭流程的状态机。为什么离线：真机上 R 只升不降，冷启动 R=0，"第 1 击把 R 钉到 0.50"要等 75 秒才看得见；这里用毫秒跑完 100 秒仿真时间 |
| `panelprobe` | 显示层的曲线点数与存储容量之间的缝（120 个存储点铺在 11 个槽位上）—— 这条缝 `storeprobe` 和 `rateprobe` 都看不到 |
| `trayprobe` | 图标资源本身：`src\tray.rc` 嵌进 exe 的 RCDATA 被加载出来，逐个 HICON 导成 PNG 叠在深浅两种底上。**裸跑 `trayprobe` 不碰通知区域** |
| `rateprobe` | 消费速率估计器与"多久清零"的措辞。估计器不取时钟、不读文件，所以只需估计器与常量表 |
| `storeprobe` | 曲线数据层：12 点环、变化才追加、`curve.json`、失效规则 |
| `beatprobe` | 心跳位移波形：包络形状、逐帧跳变、振幅与周期两条律。**它盖不到生产触发循环**（那在 `widget_display.cpp` 的 `AdvanceBeat` 里），计时器在这里是复刻的夹具 |
| `apiprobe` | 余额接口的传输、解析与错误分流，外加一个离线用例 |
| `ratebaseline` | 旧公式 vs 新公式并排跑在**真实 `curve.json`** 上（外加可选合成序列） |
| `dragprobe` | 面板自控拖动：锚点算术、面积钳制、`config.json` 及其回退。它还会驱动真窗口注入鼠标输入 |
| `coordprobe` / `tauprobe` | 坐标与常量表；Tau 探针只打印运动曲线，不碰共享状态 |

```bat
build\closeprobe
build\trayprobe
```

`build.bat` 会一起编这些目标。

---

## 现有功能

- 无边框、圆角、每像素半透明、置顶，不进任务栏、不进 `Alt+Tab`、不抢焦点。
- Per-Monitor V2 DPI 正确；`--dpi` / `--ui-scale` 可覆盖。
- 余额来自真接口（HTTPS，后台线程，默认 10 秒一次，见 `tuning.h` 的 `kApiIntervalMs`），
  带重试与错误分流；没有 Key 时进 `NoKey` 态。
- 数字有滚动动画，**滞后一拍**：显示的是上一次确认的采样，最新那次压着等下一个点。
  启动时若存储里有上次的余额，会真的滚一遍那一次变化（只在这一笔钱**真的不同**时才演）。
- 余额变化曲线：只在余额变化时记点，单调插值不过冲。
- 心跳：振幅与周期按余额危险度两条律走。
- 氛围光：颜色与光强按 R、D 走；R 只升不降。
- 关闭流程：右键进入 → 三击 → 粒子；托盘菜单「关闭」一下即关。
- 拖动与边缘吸附，位置持久化。
- 单实例：第二次启动不会再开一个挂件。

---

## License

MIT，见 `LICENSE`。
