# Pecky 帽子 + 清闲椅子完整演示命令手册

本手册按顺序执行即可完成一次稳定演示。当前固定映射为：帽子 `COM7`，椅子 `COM8`。两块设备完成开机校准后默认处于 **PAUSED**，不会产生识别事件；只有发出 `start` 后才开始测试。

椅子 `Qingxian-Chair` 的蓝牙在上电后始终广播；暂停只停止动作事件，不会关闭蓝牙，因此可随时在软件中连接或断开重连。当前帽子原型的无线启动会供电掉电，因此帽子使用终端桥接，不要选择帽子 BLE 连接。

## 0. 演示前一分钟检查

1. 帽子与椅子均通过 USB 连接电脑；帽子戴好后保持头部中立，椅子周围传感器前方没有遮挡物。
2. 打开 APP：<http://localhost:3001/>。在“我的 → 设置与数据”中分别连接 `Pecky-xxxx` 与 `Qingxian-Chair`，保持浏览器页面在前台。
3. 在第一个 **WSL 终端**启动帽子桥接服务（该终端保持运行）：

```bash
cd /home/shenicest
bash tools/start_serial_bridge_wsl.sh
```

4. 在 APP 设置中点击 **“终端桥接（推荐）→ 连接”**。APP 连接后，帽子自动 `START`；APP 点“断开”或关闭页面后，帽子自动 `PAUSE`。
5. 在第二个 WSL 终端检查状态：

```bash
cd /home/shenicest
# 桥接运行时 COM7 由桥接服务独占；椅子仍可单独检查
bash tools/control_devices_wsl.sh chair status
```

两行都应包含 `CALIBRATED=1`，并且初始状态应为 `PAUSED`。若不是，执行第 1 节。

## 1. 一键准备（推荐每次演示前执行）

先让演示者保持中立静止：帽子不要按压压力片，椅子不要坐人或保持演示起始坐姿。未启动桥接时可运行：

```bash
bash tools/control_devices_wsl.sh all pause
bash tools/control_devices_wsl.sh all calibrate
```

等待帽子校准完成（约 6 秒）和椅子校准完成（约 4 秒），再确认：

```bash
bash tools/control_devices_wsl.sh all status
```

确认两个设备均为 `PAUSED` 且 `CALIBRATED=1` 后，才开始正式展示。

> 桥接服务启动后，APP 是帽子的唯一控制端：APP 连接即开始、APP 断开即暂停；不要再用控制脚本抢占 COM7。

## 2. 正式开始与暂停

```bash
# 帽子由 APP 的“终端桥接”连接自动开始；椅子单独开始
bash tools/control_devices_wsl.sh chair start

# 演示者依次做动作；APP 会显示“已识别 · 动作名”，两秒渐隐

# 随时暂停；暂停后 APP 不会收到新的动作字幕
# APP 点击“终端桥接 → 断开”会暂停帽子；椅子单独暂停
bash tools/control_devices_wsl.sh chair pause
```

单独控制某一设备：

```bash
bash tools/control_devices_wsl.sh hat start
bash tools/control_devices_wsl.sh hat pause
bash tools/control_devices_wsl.sh chair start
bash tools/control_devices_wsl.sh chair pause
```

## 3. 建议的现场演示顺序

1. 在 APP 中展示两个设备均已连接。
2. 终端执行 `bash tools/control_devices_wsl.sh all start`。
3. 帽子演示：后仰脖子、收下巴、抱头抗阻；每个动作完成后等待 APP 字幕消失再做下一个。
4. 椅子演示：向左拉伸、向右拉伸、胸椎舒展；每个动作保持约 1 秒，动作之间回到中立约 2 秒。
5. 终端执行 `bash tools/control_devices_wsl.sh all pause`，并用 `status` 展示两端已经暂停。

## 4. APP 启动命令

若 APP 没有运行，在 **WSL 终端**（提示符形如 `iboy@DESKTOP...:/home/shenicest$`）执行：

```bash
cd /home/shenicest
npm run dev -- --host 0.0.0.0
```

浏览器打开终端显示的本地地址。开发机默认通常是 <http://localhost:3000/>；若端口已被占用，使用终端实际输出的地址，例如 <http://localhost:3001/>。

> 所有日常演示命令都可在同一个 WSL 终端执行。控制包装脚本会调用 Windows 的串口层访问 `COM7`、`COM8`，无需再打开 PowerShell。

## 5. 常见问题与恢复

### `无法连接 COM7` 或 `COM8`

关闭 Arduino IDE 串口监视器、PuTTY 等占用串口的软件，然后检查：

```bash
bash tools/control_devices_wsl.sh all status
```

确认 COM7 是帽子、COM8 是椅子后重试。

### `CALIBRATED=0` 或识别不稳定

暂停后重新校准。校准期间必须静止；帽子不要按压力片，椅子传感器前方不要有人移动。

```bash
bash tools/control_devices_wsl.sh all pause
bash tools/control_devices_wsl.sh all calibrate
```

### APP 没有字幕

依次检查：设备已经在 APP 中连接、终端 `status` 显示 `RUNNING`、并且动作持续足够明显。可执行：

```bash
bash tools/control_devices_wsl.sh all status
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
