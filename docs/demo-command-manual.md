# Pecky 帽子 + 清闲椅子完整演示命令手册

本手册按顺序执行即可完成一次稳定演示。当前固定映射为：帽子 `COM7`，椅子 `COM8`。两块设备完成开机校准后默认处于 **PAUSED**，不会产生识别事件；只有发出 `start` 后才开始测试。

## 0. 演示前一分钟检查

1. 帽子与椅子均通过 USB 连接电脑；帽子戴好后保持头部中立，椅子周围传感器前方没有遮挡物。
2. 打开 APP：<http://localhost:3001/>。在“我的 → 设置与数据”中分别连接 `Pecky-xxxx` 与 `Qingxian-Chair`，保持浏览器页面在前台。
3. **另开一个 Windows PowerShell 窗口**（不要在 `iboy@...:/home/...$` 的 WSL 提示符中执行），进入项目目录：

```powershell
cd "\\wsl.localhost\Ubuntu-22.04\home\shenicest"
py -3 tools\control_devices.py all status
```

两行都应包含 `CALIBRATED=1`，并且初始状态应为 `PAUSED`。若不是，执行第 1 节。

## 1. 一键准备（推荐每次演示前执行）

先让演示者保持中立静止：帽子不要按压压力片，椅子不要坐人或保持演示起始坐姿。然后依次运行：

```powershell
py -3 tools\control_devices.py all pause
py -3 tools\control_devices.py all calibrate
```

等待帽子校准完成（约 6 秒）和椅子校准完成（约 4 秒），再确认：

```powershell
py -3 tools\control_devices.py all status
```

确认两个设备均为 `PAUSED` 且 `CALIBRATED=1` 后，才开始正式展示。

## 2. 正式开始与暂停

```powershell
# 同时开始帽子和椅子识别
py -3 tools\control_devices.py all start

# 演示者依次做动作；APP 会显示“已识别 · 动作名”，两秒渐隐

# 随时暂停；暂停后 APP 不会收到新的动作字幕
py -3 tools\control_devices.py all pause
```

单独控制某一设备：

```powershell
py -3 tools\control_devices.py hat start
py -3 tools\control_devices.py hat pause
py -3 tools\control_devices.py chair start
py -3 tools\control_devices.py chair pause
```

## 3. 建议的现场演示顺序

1. 在 APP 中展示两个设备均已连接。
2. 终端执行 `py -3 tools\control_devices.py all start`。
3. 帽子演示：后仰脖子、收下巴、抱头抗阻；每个动作完成后等待 APP 字幕消失再做下一个。
4. 椅子演示：向左拉伸、向右拉伸、胸椎舒展；每个动作保持约 1 秒，动作之间回到中立约 2 秒。
5. 终端执行 `py -3 tools\control_devices.py all pause`，并用 `status` 展示两端已经暂停。

## 4. APP 启动命令

若 APP 没有运行，在 **WSL 终端**（提示符形如 `iboy@DESKTOP...:/home/shenicest$`）执行：

```bash
cd /home/shenicest
npm run dev -- --host 0.0.0.0
```

浏览器打开终端显示的本地地址。开发机默认通常是 <http://localhost:3000/>；若端口已被占用，使用终端实际输出的地址，例如 <http://localhost:3001/>。

> 设备控制命令和 APP 启动命令需要在不同终端执行：WSL 默认不能直接访问 Windows 的 `COM7`、`COM8`；因此 `py -3 tools\control_devices.py ...` 必须在 Windows PowerShell 中执行。

## 5. 常见问题与恢复

### `无法连接 COM7` 或 `COM8`

关闭 Arduino IDE 串口监视器、PuTTY 等占用串口的软件，然后检查：

```powershell
arduino-cli board list
py -3 tools\control_devices.py all status
```

确认 COM7 是帽子、COM8 是椅子后重试。

### `CALIBRATED=0` 或识别不稳定

暂停后重新校准。校准期间必须静止；帽子不要按压力片，椅子传感器前方不要有人移动。

```powershell
py -3 tools\control_devices.py all pause
py -3 tools\control_devices.py all calibrate
```

### APP 没有字幕

依次检查：设备已经在 APP 中连接、终端 `status` 显示 `RUNNING`、并且动作持续足够明显。可执行：

```powershell
py -3 tools\control_devices.py all status
```

若 APP 蓝牙连接断开，在“我的 → 设置与数据”重新连接对应设备；不需要重新烧录固件。

### 重新烧录固件（仅在需要更新时）

```powershell
$cli = 'C:\Users\iboy\AppData\Local\Microsoft\WinGet\Packages\ArduinoSA.IDE.stable_Microsoft.Winget.Source_8wekyb3d8bbwe\resources\app\lib\backend\resources\arduino-cli.exe'
& $cli compile --fqbn esp32:esp32:esp32s3 firmware/04_hat_recognition_ble
& $cli upload -p COM7 --fqbn esp32:esp32:esp32s3 firmware/04_hat_recognition_ble
& $cli compile --fqbn esp32:esp32:esp32s3 Chair/firmware/01_chair_recognition_ble
& $cli upload -p COM8 --fqbn esp32:esp32:esp32s3 Chair/firmware/01_chair_recognition_ble
```

烧录后必须重新执行第 1 节的校准流程。
