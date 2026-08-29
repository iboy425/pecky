# Pecky PWA（中文说明）

- 项目仓库：<https://github.com/iboy425/pecky>
- 硬件物料清单：[`docs/BOM.md`](docs/BOM.md)
- GitHub Topic：`shenicest-fission`

Pecky 是面向 Pecky 硬件概念的移动优先储蓄伙伴。它把硬件触发的“啄击”事件
转换为共享储蓄罐余额，将余额与多个愿望进行比较，并把已完成的愿望保存为个人成就。

## 产品行为

- 储蓄罐与开场动画位于同一页面。启动时会领取尚未展示的待处理事件，在开场体验中
  展示一次，随后收起为静态储蓄罐页面。
- 开场收起后，储蓄罐主视觉会播放小鸡绕罐动画；余额文字始终显示在左侧独立区域。
- 开场不能手动重播。事件会在播放前持久化领取状态，因此刷新页面或程序崩溃都不会
  重播同一批事件。
- 一个共享余额会与所有未完成愿望进行比较，愿望按目标价格排序。
- 余额充足时才能标记为已购买。确认购买后会扣除目标金额、移除愿望并新增一条历史记录。
- 累计储蓄金额和累计啄击次数在购买后不会减少。
- “我的”页面包含个人资料、累计数据、成就、购买历史及数据设置。
- 第一版使用本地 IndexedDB 存储，并可通过 Web Bluetooth 直接连接 Pecky 帽子；
  不包含账号或云端同步。

## 硬件事件协议

模拟器、JSON 导入器与实时 BLE 适配器使用同一个数据源接口，定义见
`app/lib/sources.ts`。BLE 完成事件会保留识别到的帽子动作：`neck_extension`、
`chin_tuck` 或 `head_resistance`。

应用也可独立连接人体工学椅：椅子动作不会改变储蓄罐余额，而是显示一个持续两秒、
随后淡出的“已识别”提示。

```json
{
  "version": 1,
  "events": [
    {
      "eventId": "event-001",
      "deviceId": "PECKY-001",
      "sequence": 1,
      "peckCount": 10,
      "amountDelta": 10,
      "occurredAt": "2026-08-28T10:00:00.000Z"
    }
  ]
}
```

系统会对 `eventId` 以及 `deviceId + sequence` 组合去重。金额按最小货币单位的整数存储。
IndexedDB 更新使用串行读写事务，避免导入和购买操作在多标签页中互相覆盖。

## 演示与首次使用

- 开发模式会在空浏览器配置中写入已批准的演示状态。
- 正式环境的首次访问从 ¥0 开始。
- 在生产 URL 后添加 `?demo=1`，可在空浏览器配置中写入演示数据。
- 设置中的“加载演示数据”会在确认后替换当前本地状态。
- “清除本地数据”会写入空状态并禁用演示数据自动写入，避免下次启动重新出现演示内容。

## 测试开场流程

1. 打开 **我的 → 设置与数据**。
2. 输入啄击次数和金额，然后选择 **模拟并播放开场**。
3. 事件会通过与未来硬件一致的适配器协议写入；页面随后以全新启动状态展示一次开场。
4. 开场收起至储蓄罐页面后再次刷新，确认它不会重播；这是设计使然。

如果选择 **模拟数据**，事件只会被记录；刷新页面或完全重启已安装的 PWA 后，才会看到
一次性的开场体验。

## 本地开发

要求：Node.js 22.13 或更高版本。

```bash
npm install
npm run dev
npm run prepare-assets
npm run lint
npm test
```

帽子固件使用 `NimBLE-Arduino` 2.5.x。编译 `firmware/04_hat_recognition_ble`
前请先安装一次：

```powershell
arduino-cli lib install "NimBLE-Arduino@2.5.1"
```

## 真实硬件启动

将现有帽子识别固件烧录到帽子 ESP32-S3，将椅子识别固件烧录到椅子 ESP32-S3。每个设备
首次校准时保持静止。两者各自广播独立的 BLE 服务，可在 Android Chrome 或已安装的 Android
PWA 的 **我的 → 设置与数据** 中分别连接。

```bash
# 在仓库根目录的 WSL / Ubuntu 环境中执行
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/04_hat_recognition_ble
arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32s3 firmware/04_hat_recognition_ble

arduino-cli compile --fqbn esp32:esp32:esp32s3 Chair/firmware/01_chair_recognition_ble
arduino-cli upload -p /dev/ttyUSB1 --fqbn esp32:esp32:esp32s3 Chair/firmware/01_chair_recognition_ble

# 启动网页应用；手机需使用开发服务器显示的 HTTPS 地址
npm run dev -- --host 0.0.0.0
```

请使用 `arduino-cli board list` 显示的实际串口路径；不要假定所有电脑都使用
`/dev/ttyUSB0` 和 `/dev/ttyUSB1`。Web Bluetooth 需要安全上下文：除 localhost 外需使用
HTTPS，并且浏览器必须支持 Web Bluetooth。

### 终端控制的测试场次

两个设备启动并完成校准后默认处于 **暂停** 状态。使用控制器明确决定何时开始、暂停或重新
校准测试。暂停状态不会产生 BLE 动作通知，因此应用不会显示提示，也不会新增帽子储蓄事件。

椅子的蓝牙在暂停识别时仍保持启用：`Qingxian-Chair` 会持续可发现，并在应用断开后自动重新
连接。当前帽子原型因启动时无线供电不稳定，继续采用已验证的 USB 事件流；在修复其 5V 供电
和去耦前，请不要在应用中选择帽子 BLE 选项。

```bash
# WSL 终端：封装脚本会访问 Windows 的 COM7 / COM8
bash tools/control_devices_wsl.sh all status
bash tools/control_devices_wsl.sh all start
bash tools/control_devices_wsl.sh hat pause
bash tools/control_devices_wsl.sh chair calibrate
```

关于校准、推荐动作顺序、连接恢复与重新烧录的完整现场流程，请参阅
[中文演示命令手册](docs/demo-command-manual.md)。

### 推荐的帽子连接方式：终端桥接

帽子的 COM7 由专用终端桥接程序独占，避免浏览器串口权限和终端控制互相竞争。在一个 WSL
终端中启动并保持运行：

```bash
bash tools/start_serial_bridge_wsl.sh
```

然后在应用中选择 **终端桥接（推荐）→ 连接**。连接会开始识别并把动作事件写入储蓄罐；
断开连接会自动暂停识别。

`npm test` 会构建 vinext/Cloudflare 输出，并验证渲染后的 PWA 外壳、生产元数据、开场状态机、
持久化协议、适配器以及必需的网页资源。

## 项目结构

- `app/components/PeckyApp.tsx`：移动端界面、交互状态与开场过渡。
- `app/lib/model.ts`：金额、愿望、购买、成就和事件规则。
- `app/lib/storage.ts`：原子化 IndexedDB 持久化与跨标签页刷新。
- `app/lib/sources.ts`：模拟器、JSON 导入与实时 Web Bluetooth 适配器。
- `public/manifest.webmanifest` 与 `public/sw.js`：可安装、离线可用的 PWA 外壳。
- `scripts/prepare-assets.mjs`：只读地将已批准源图转换为网页运行时资源。

已批准的源图保持在项目外部、不被修改；运行时派生资源存放在 `public/assets`。
