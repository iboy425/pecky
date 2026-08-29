# Pecky 帽子 + 清闲椅子完整演示命令手册

本手册按顺序执行即可完成一次稳定演示。当前固定映射为：帽子 `COM7`，椅子 `COM8`。两块设备完成开机校准后默认处于 **PAUSED**，不会产生识别事件；只有发出 `start` 后才开始测试。

帽子和椅子均使用 **终端桥接**：浏览器不直接配对蓝牙，也不会占用 COM7/COM8。两台设备可在同一 APP 设置页中分别连接、断开，或同时连接；每张卡只控制自己的设备。

## 0. 演示前一分钟检查

1. 帽子与椅子均通过 USB 连接电脑；帽子戴好后保持头部中立，椅子周围传感器前方没有遮挡物。
2. 打开 APP：<http://localhost:3001/>，进入“我的 → 设置与数据”，暂时不要点设备连接。
3. 在第一个 **WSL 终端**启动帽子桥接服务（该终端保持运行）：

```bash
cd /home/shenicest
bash tools/start_serial_bridge_wsl.sh
```

4. 在 APP 设置中点击 **“终端桥接（推荐）→ 连接”**。APP 连接后，帽子自动 `START`；APP 点“断开”或关闭页面后，帽子自动 `PAUSE`。
5. 在第二个 **WSL 终端**启动椅子桥接服务（该终端保持运行）：

```bash
cd /home/shenicest
bash tools/start_chair_serial_bridge_wsl.sh
```

6. 回到同一个 APP 设置页：可独立点击“终端桥接（推荐）”连接帽子，点击“清闲椅子终端桥接”连接椅子；两张卡可同时显示已连接。每次连接会自动 `START` 对应设备，断开对应卡或关闭页面会自动 `PAUSE` 对应设备。

7. 桥接运行时串口由桥接服务独占。需要检查状态时，先在 APP 断开对应设备，再执行：

```bash
cd /home/shenicest
bash tools/control_devices_wsl.sh all status
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

> 两个桥接服务启动后，APP 是对应设备的唯一控制端：APP 连接即开始、APP 断开即暂停；不要再用控制脚本抢占已桥接的 COM7/COM8。

## 2. 正式开始与暂停

```bash
# 分别在 APP 的两张终端桥接卡上点击“连接”：帽子、椅子可任选其一或同时连接。
# 演示者动作完成后，APP 会加米并显示“已识别 · 动作名”，两秒渐隐。
# 点击某一张卡的“断开”，只会暂停那一台设备，另一台仍可继续工作。
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
2. 在 APP 中连接帽子、椅子两张终端桥接卡。
3. 帽子演示：后仰脖子、收下巴、抱头抗阻；每个动作完成后等待 APP 字幕消失再做下一个。
4. 椅子演示：向左拉伸、向右拉伸、胸椎舒展；每个动作保持约 1 秒，动作之间回到中立约 2 秒。
5. 在 APP 中分别点击两张卡的“断开”；此时两台设备都暂停。

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

若 APP 连接断开，在“我的 → 设置与数据”重新连接对应的终端桥接卡；不需要重新烧录固件。

### 重新烧录固件（仅在需要更新时）

```powershell
$cli = 'C:\Users\iboy\AppData\Local\Microsoft\WinGet\Packages\ArduinoSA.IDE.stable_Microsoft.Winget.Source_8wekyb3d8bbwe\resources\app\lib\backend\resources\arduino-cli.exe'
& $cli compile --fqbn esp32:esp32:esp32s3 firmware/04_hat_recognition_ble
& $cli upload -p COM7 --fqbn esp32:esp32:esp32s3 firmware/04_hat_recognition_ble
& $cli compile --fqbn esp32:esp32:esp32s3 Chair/firmware/01_chair_recognition_ble
& $cli upload -p COM8 --fqbn esp32:esp32:esp32s3 Chair/firmware/01_chair_recognition_ble
```

烧录后必须重新执行第 1 节的校准流程。
