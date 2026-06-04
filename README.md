# Clawd Mochi Tank 状态灯/状态屏

这是一个把 Codex/Claude 的工作状态显示到 ESP32 小设备上的项目。

设备有两个固件版本：

- `src/main.cpp`：三颗 LED 版本，适合 ESP32-C3 SuperMini。
- `ue/src/main.cpp`：240x240 ST7789 彩屏版本，适合普通 ESP32 开发板。

简单理解：电脑上的 Hook Hub 收到 Codex 状态，然后通过 BLE 或串口发给 ESP32，ESP32 播放对应动画或点亮对应灯效。

## 工作流程

```text
Codex / Claude
  -> 本机 Hook 或 session watcher
  -> Hook Hub: http://127.0.0.1:8765
  -> BLE 或 USB 串口
  -> ESP32 设备
```

## 需要准备

- VS Code 或 PlatformIO。
- 一块 ESP32 板子。
- 如果刷彩屏版本，需要 ST7789 240x240 屏幕。
- Python 3.10+，用于运行 Hook Hub。
- 可选 Python 包：

```powershell
python -m pip install pyserial bleak
```

`pyserial` 用来走 USB 串口，`bleak` 用来走 BLE。

## 目录说明

```text
src/main.cpp                         LED 版本固件
ue/src/main.cpp                      ST7789 彩屏版本固件
platformio.ini                       LED 版本 PlatformIO 配置
ue/platformio.ini                    彩屏版本 PlatformIO 配置
skills/codex-clawd-status/           Codex 状态桥接脚本
skills/claude-clawd-status/          Claude 状态桥接脚本
include/tank_assets/                 彩屏动画资源
```

## 刷入 LED 版本

在项目根目录执行：

```powershell
pio run
pio run -t upload
```

打开串口监视器：

```powershell
pio device monitor
```

LED 版本默认板子配置是：

```text
esp32-c3-devkitm-1
monitor_speed = 115200
```

三颗 LED 默认接线在代码里：

```text
RUN   -> GPIO0
WAIT  -> GPIO1
ALERT -> GPIO2
```

代码按共阳极 LED 写的：GPIO 输出 LOW 时 LED 亮。

## 刷入彩屏版本

进入 `ue` 目录：

```powershell
cd ue
pio run
pio run -t upload
pio device monitor
```

彩屏版本默认 ST7789 接线：

```text
VCC  -> 3V3
GND  -> GND
SCK  -> GPIO22
MOSI -> GPIO21
RST  -> GPIO17
DC   -> GPIO16
CS   -> GPIO18
BL   -> GPIO26
```

## 设备启动后会发生什么

开机后设备会：

1. 不启动 WiFi。
2. 启动 BLE，名字是 `Claude-Mochi-Tank`。
3. 默认进入 `beacon` 状态，表示等待连接或等待状态。

当前固件已经删除 WiFi/AP/WebServer 相关代码，只通过 BLE 或 USB 串口接收状态。

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
sleeping      动画上的睡觉
dizzy         错误
disconnected 连接断开
```

注意：`sleeping` 是动画状态，不一定等于 ESP32 板子真的休眠。

## 低功耗逻辑

当前固件的逻辑是：

```text
收到有效状态
  -> 记录 lastCommandMs
  -> 播放对应动画或灯效
  -> 3 分钟没有新的有效状态
  -> 进入 lowPowerMode
  -> 关闭 LED 或屏幕背光
  -> ESP32 进入 light sleep
  -> 收到新的有效状态
  -> 退出 lowPowerMode
```

当前使用的是 ESP32 `light sleep`，不是 `deep sleep`。

原因是 deep sleep 会让板子重启，而且 BLE 不能保持正常接收；这个项目需要“新状态来了就恢复”，所以用 light sleep 更合适。

当前实现里 low power 会每 250ms 定时醒来一次，检查串口和 BLE。如果没有新状态，会继续睡。

## 手动发送测试命令

串口里可以输入：

```text
anim=typing
anim=thinking
anim=building
anim=happy
anim=sleeping
next
state
```

也可以发送 JSON：

```json
{"anim":"typing"}
{"anim":"building"}
{"backlight":false}
{"speed":3}
```

LED 版本还支持：

```text
led=100
led=010
led=001
led=111
```

## 启动 Codex Hook Hub

安装 Codex hooks：

```powershell
C:\Python314\python.exe C:\Users\admin\.codex\skills\codex-clawd-status\scripts\install_hooks.py
```

然后重启 Codex，并在 Codex 里运行：

```text
/hooks
```

确认并信任 hook。

日常启动 Hub App：

```powershell
Start-Process -FilePath "C:\Python314\python.exe" `
  -ArgumentList @(
    "C:\Users\admin\.codex\skills\codex-clawd-status\scripts\clawd_hub_app.py",
    "--minimized"
  ) `
  -WindowStyle Hidden
```

打开 Hub 页面：

```text
http://127.0.0.1:8765
```

## 测试 Hub 和设备连接

检查设备发现：

```powershell
C:\Python314\python.exe C:\Users\admin\.codex\skills\codex-clawd-status\scripts\codex_clawd_hook.py --doctor
```

发送一个测试状态：

```powershell
C:\Python314\python.exe C:\Users\admin\.codex\skills\codex-clawd-status\scripts\codex_clawd_hook.py --test thinking
```

查看 Hub 状态：

```powershell
Invoke-RestMethod http://127.0.0.1:8765/state
```

## 如果设备没有变化

先按这个顺序排查：

1. 设备有没有成功刷入固件。
2. 串口监视器里有没有启动日志。
3. Hook Hub 页面 `http://127.0.0.1:8765` 是否能打开。
4. Hub 页面有没有收到事件。
5. `--doctor` 是否能找到 BLE 或串口设备。
6. 如果串口被 PlatformIO Serial Monitor 占用，Hub 可能不能再用串口发送。
7. 如果 BLE 不稳定，可以先试 USB 串口。
8. 当前固件没有 WiFi AP 和网页控制，不要用 `192.168.4.1` 判断设备是否在线。

## 如果低功耗没有生效

重点检查：

1. 是否真的超过 3 分钟没有新状态。
2. Hub 或 watcher 是否还在不断发送状态。
3. 串口里是否打印了 `command_timeout_low_power`。
4. LED 版本里如果最后状态是 `sleeping` 或 `going_away`，旧逻辑可能会阻止进入板级低功耗，需要留意当前代码状态。

## 常用编译命令

根目录 LED 版本：

```powershell
pio run
pio run -t upload
pio device monitor
```

`ue` 彩屏版本：

```powershell
cd ue
pio run
pio run -t upload
pio device monitor
```

## 给新手的一句话

先让 ESP32 能刷入并在串口里看到启动日志，再打开 Hook Hub 页面，最后用 `--test thinking` 测试状态能不能送到设备。这个链路通了以后，Codex 的真实工作状态才会自然显示到设备上。
