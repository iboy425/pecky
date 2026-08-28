# 帽子三动作数据采集

这份流程只使用当前帽子的硬件：ESP32-S3、帽内 IMU，以及已经接好的单个
RFP602 压力通道。目标不是立刻硬编码阈值，而是先得到带时间标签的真实样本，
再为这顶帽子和这位佩戴者调出可靠的动作判定。

## RST 作为采集开关

这版固件把帽子在电池、USB 或电脑之间切换时都保持为**空闲**，不会自动新建文件。
只把物理 `RST` 键当作一位受试者采集的开关：

1. 帽子上电后看到 `INFO,CAPTURE_IDLE`，先戴好帽子；此时尚未记录。
2. **按一次 `RST`：开始本人的采集。** 重启后前三秒保持自然坐姿、头部正对前方，
   用于陀螺仪和中立姿态校准；随后自动创建一个新 CSV，例如 `/hat_0012.csv`。
3. 按固定动作脚本完成三类动作后，安静等待约一秒。
4. **再按一次 `RST`：结束本人的采集。** 设备回到空闲，不会生成空白新文件。
5. 换下一位受试者时，再按一次 `RST` 开始；结束时再按一次 `RST` 停止。

记录频率为 25 Hz，数据每 250 ms 落盘一次；按 RST 或意外断电最多丢失最后约
四分之一秒。不要用拔电源来区分参与者。

每一行包含原始加速度、原始陀螺仪、压力原始值和当前阶段编号。
原始数据比“已算好的次数”更有价值，后续可以重复调参数。

## 一次采集的固定动作脚本

从 **开始自动记录** 的时刻启动手机秒表；先后做下列动作。每个动作都慢做、回到
中立、不要借助身体大幅前后移动。一次重置对应一位受试者的一份独立文件。

| 秒表时间 | 固件标签 | 你要做什么 |
|---|---|---|
| 0–5 秒 | `0 / neutral_start` | 正常坐姿，头部中立，不做动作。 |
| 5–17 秒 | `1 / neck_extension` | 后仰脖子 3 次：每次到位保持约2秒，再慢慢回中立。 |
| 17–22 秒 | `2 / neutral_1` | 正常坐姿，作为负样本。 |
| 22–34 秒 | `3 / chin_tuck` | 收下巴 3 次：头保持水平向后收，不低头、不点头；每次保持约2秒。 |
| 34–39 秒 | `4 / neutral_2` | 正常坐姿，作为负样本。 |
| 39–54 秒 | `5 / head_resistance` | 双手抱后脑勺抗阻 2 次：中立位用力保持5秒，再完全放松。 |
| 54–60 秒 | `6 / normal_motion` | 低头、点头、左右转头、耸肩、自然调整；这些必须不计数。 |
| 60 秒后 | `7 / free` | 不要继续走动或聊天；等待约一秒后按 RST 结束本人的记录。 |

建议找 **10 位不同受试者**，每人完整做这60秒，然后按一次 `RST` 结束；下一位再
按一次 `RST` 开始。
文件会依次变成 `/hat_0001.csv`、`/hat_0002.csv` ……；十位受试者的数据能同时保留在
芯片内。

## 记录前的硬件检查

串口开机日志应有：

```text
INFO,WHO_AM_I,0x70
INFO,CAPTURE_IDLE,PRESS_RST_ONCE_TO_START
```

开始采集后会依次出现：

```text
INFO,RST_TOGGLE,STARTING_KEEP_STILL_FOR_CALIBRATION
INFO,LOGGING_STARTED,/hat_0012.csv
```

结束采集后应显示：

```text
INFO,RST_TOGGLE,STOPPED
```

`0x70` 对应当前已确认的 MPU6500 兼容模块，是正常的。压力传感器会记录在
`pressure_raw` 列：现有三位受试者的数据已验证，在抗阻段出现了明显的非零压力值。

如果连续看到 `WARN,PRESSURE_CHANNEL_SATURATED_OR_DISCONNECTED`，也照样采集
IMU 数据；但请标注出来。此前压力值长期为 `4095` 说明 ADC 被 3.3V 顶满，之后
需要修复分压接法：`3.3V → RFP602 → ADC(GPIO4) → 10kΩ → GND`。

## 下载数据

完成一个会话后，给帽子断电。之后用 USB 接回电脑、关闭 PuTTY 或 Arduino 串口监视器，
在 Windows PowerShell 执行：

```powershell
cd \\wsl.localhost\Ubuntu-22.04\home\shenicest
C:\Users\iboy\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe tools\download_hat_flash_log.py --port COM7 --all
```

工具会下载全部已完成记录到 `data/raw/hat_XXXX.csv`。插 USB 不会自动开启新的
采集；若只想查看芯片内文件：

```powershell
C:\Users\iboy\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe tools\download_hat_flash_log.py --port COM7 --list
```

下载的 CSV 只保存在本地，不会提交到 GitHub。把文件路径发给我，我会基于真实数据
计算每个动作的候选阈值，并把识别程序改为只给这三种动作计数。
