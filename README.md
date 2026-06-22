# Clawd Mochi Tank 状态灯

把 Codex/Claude 的工作状态显示到 ESP32-C3 SuperMini 三颗 LED 上。

简单理解：电脑上的 Hook Hub 收到 Codex/Claude 状态，通过 USB 串口发给 ESP32，ESP32 点亮对应灯效。

## 工作流程

```text
Codex / Claude
  -> 本机 Hook 或 session watcher
  -> Hook Hub: http://127.0.0.1:8765
  -> USB 串口
  -> ESP32-C3 SuperMini
```

## 需要准备

- PlatformIO（VS Code 插件或 CLI）。
- ESP32-C3 SuperMini 开发板。
- Python 3.10+，用于运行 Hook Hub。

```powershell
python -m pip install pyserial
```

## LED 接线

三颗 LED 默认接线（共阳极，GPIO 输出 LOW 时亮）：

```text
RUN   -> GPIO0
WAIT  -> GPIO1
ALERT -> GPIO2
```

## 编译与刷入

```powershell
pio run
pio run -t upload
pio device monitor
```

默认板子配置：

```text
esp32-c3-devkitm-1
monitor_speed = 115200
```

## 常见状态含义

```text
beacon        等待连接或等待新状态
idle          空闲
typing        正在输入/编辑
thinking      思考中
building      正在构建或运行命令
debugger      调试/检查
wizard        网络、图片生成等特殊动作
confused      等待确认或需要注意
alert         提醒
happy         任务完成
sleeping      睡眠动画
dizzy         错误
disconnected  连接断开
```

## 手动发送测试命令

串口里可以输入：

```text
anim=typing
anim=thinking
anim=building
anim=happy
anim=sleeping
state
```

也可以发送 JSON：

```json
{"anim":"typing"}
{"anim":"building"}
{"speed":3}
```

LED 直接控制：

```text
led=100
led=010
led=001
led=111
```

## 低功耗逻辑

```text
收到有效状态
  -> 播放对应灯效
  -> 收到 sleeping / going_away 状态 -> 关闭 LED，ESP32 保持运行
  -> 收到新的有效状态 -> 立即更新 LED
```

固件不进入 `light sleep` 或 `deep sleep`；收到新状态后可立即更新 LED。

## 启动 Hook Hub

安装 Claude hooks：

```powershell
C:\Python314\python.exe C:\Users\admin\.claude\skills\claude-clawd-status\scripts\install_hooks.py
```

日常启动 Hub：

```powershell
Start-Process -FilePath "C:\Python314\python.exe" `
  -ArgumentList @(
    "C:\Users\admin\.claude\skills\claude-clawd-status\scripts\clawd_hub_app.py",
    "--minimized"
  ) `
  -WindowStyle Hidden
```

打开 Hub 页面：`http://127.0.0.1:8765`

发送测试状态：

```powershell
C:\Python314\python.exe C:\Users\admin\.claude\skills\claude-clawd-status\scripts\claude_clawd_hook.py --test thinking
```

## 排查问题

1. 确认固件已成功刷入，串口有启动日志。
2. 确认 Hub 页面 `http://127.0.0.1:8765` 能打开并收到事件。
3. 如果串口被 PlatformIO Serial Monitor 占用，Hub 无法同时使用串口发送。
4. 当前固件无 WiFi/BLE，只通过 USB 串口接收状态。
