# deepseek-balance

DeepSeek 账户余额挂件。

显示你的 DeepSeek 账户余额，实时更新。

---

## 进度

按 `不入库文件\v0.2 实施步骤.md` 的步骤推进（该目录不入库）。已完成：

| 步骤 | 内容 | 状态 |
| --- | --- | --- |
| A0 | 渲染路径裁决实验 | 完成。结论：**DirectComposition + D3D11 翻转模型**。两路视觉一致（所有者目视确认），代价差约 15 倍（p50 3.66ms → 0.24ms） |
| A1 | 能编译出一个空窗口 | 完成。`build\dshb.exe`，静态 CRT，依赖只有 `USER32/GDI32/KERNEL32` |
| A2 | 构建脚本固化 | 完成。`build.bat`：`vcvars64` → `cmake -G Ninja`，脚本内禁用 `set PATH=` |
| A3 | Per-Monitor V2 DPI 声明 | 完成。实测 200% 缩放下窗口 950×578、实体区 630×258 |
| A3b | DPI 相关 API 全部换带后缀版本 | 完成。旧名字检索命中数为 0 |
| A3c | 无边框 + 圆角 + 置顶 + 每像素透明 | 完成。**六条目视验收全过**：无边框、透出壁纸、圆角真透、帧循环在跑、点击不抢焦点、不在任务栏与 Alt+Tab |
| A4b | 画布外扩 + 透明区是否吃鼠标 | 完成。**DirectComposition 的窗口矩形整体都会吃鼠标事件**（不按 alpha 做命中测试），已用 `SetWindowRgn` 把可点区域收到实体区；实测余量全部穿透、且区域不裁画面 |
| A5b | 离屏导帧 | 完成。`--export-frame=N --out=x.png` 用同一份绘制代码渲染到离屏位图；同一帧两次导出逐字节相同，像素量与设计一致（这把"用像素说话"的尺子校准好了） |

### 怎么跑

```
build.bat    构建（双击即可）
run.bat      看一眼（面板出现 15 秒，Esc 提前关）
```

构建前提：本机已装 VS Build Tools（MSVC 14.51 + Windows SDK 10.0.26100）。**`build.bat` 里不能出现任何 `set PATH=`**——那会静默丢掉 MSVC 工具链，然后 cmake 报找不到编译器。

### 运行需要的东西

密钥从环境变量 `DEEPSEEK_API_KEY` 读取，只在内存里用，不落盘。尚未接入真实接口（当前是骨架，还没有数据管道）。

## License

MIT
