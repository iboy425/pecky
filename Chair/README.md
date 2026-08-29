# 清闲人体工学椅（Chair）

本目录只存放“清闲”人体工学椅项目的代码和说明，与仓库中原有的帽子项目相互独立。

当前阶段的目标是先跑通硬件链路：

- 5 路 HC-SR04 超声波测距；
- 1 路 GY-521 / MPU-6050 六轴传感器；
- 1 路 DHT11 臀部微环境温湿度传感器；
- 1 路三针无源蜂鸣器；
- ESP32-S3-N16R8 主控；
- 串口固定为 `COM8`，不枚举或访问其他串口。

风扇不在当前范围内。前三个动作的识别率需要在硬件通过后，通过真实数据采集、按参与者隔离训练/测试和盲测验证；本固件只负责证明各传感器通道可用。

动作数据采集阶段只使用 5 路超声、MPU 和蜂鸣器，不读取 DHT11；
DHT11 只保留在独立的硬件自检固件中。

## 目录

```text
Chair/
  README.md
  firmware/
    hardware_bringup/
      hardware_bringup.ino
    data_collection/
      data_collection.ino
  tools/
    check_com8.py
    capture_actions_com8.py
    capture_left_com8.sh
    capture_right_com8.sh
    capture_chest_com8.sh
    run_capture_com8.sh
  data/
    .gitignore
```

## 固定引脚

| 通道 | 位置 | ESP32-S3 接线 |
|---|---|---|
| HC1 | 坐者左外 | TRIG=GPIO4，ECHO=GPIO5 |
| HC2 | 坐者左内 | TRIG=GPIO6，ECHO=GPIO7 |
| HC3 | 胸椎中央 | TRIG=GPIO10，ECHO=GPIO11 |
| HC4 | 坐者右内 | TRIG=GPIO15，ECHO=GPIO16 |
| HC5 | 坐者右外 | TRIG=GPIO17，ECHO=GPIO18 |
| MPU-6050 | 椅背中央活动支撑 | SDA=GPIO8，SCL=GPIO9，AD0=GND |
| DHT11 | 坐垫下方通风区域 | DATA=GPIO12 |
| 无源蜂鸣器 | 椅背中央 | S=GPIO13（串联 100Ω） |

每个 HC-SR04 的 ECHO 必须独立分压：

```text
HC-SR04 ECHO -- 10kΩ --+-- ESP32 ECHO GPIO
                       |
                      20kΩ
                       |
                      GND
```

HC-SR04 使用 5V；MPU-6050、DHT11 和蜂鸣器使用 3.3V；所有模块共地。

## 串口自检协议

固件使用 `115200 8N1`。启动后会输出：

```text
CHECK,HC1_LEFT_OUTER,PASS,DIST_CM=...
CHECK,HC2_LEFT_INNER,PASS,DIST_CM=...
CHECK,HC3_CENTER,PASS,DIST_CM=...
CHECK,HC4_RIGHT_INNER,PASS,DIST_CM=...
CHECK,HC5_RIGHT_OUTER,PASS,DIST_CM=...
CHECK,MPU6050,PASS,...
CHECK,DHT11,PASS,TEMP_C=...,HUMIDITY_RH=...
CHECK,BUZZER,MANUAL_CONFIRM,TONE_SENT
SUMMARY,PASS=...,FAIL=...
```

串口命令：

- `R`：重新执行全部硬件自检；
- `B`：蜂鸣器再响一次；
- `1`～`5`：单独检测对应 HC-SR04；
- `X`：依次触发每个 TRIG，同时监听全部 5 路 ECHO，并输出 GPIO
  高低回读和弱上拉探测结果，用于定位错位、短路或浮空；
- `Y`：测试一路 TRIG 时将其他 TRIG 设为高阻，用于判断多路 TRIG
  是否被意外并接或互相拉低；
- `M`：单独检测 MPU-6050；
- `D`：单独检测 DHT11。

## 只检查 COM8

安装仓库根目录 `requirements.txt` 中的 `pyserial` 后运行：

```powershell
python Chair\tools\check_com8.py --seconds 25
```

该脚本不会列出串口，也不会自动选择串口；代码中只允许打开 `COM8`。

## 三动作数据采集

`data_collection.ino` 上电后立即以约 6.25 帧/秒输出 5 路超声和
MPU 数据。固件不读取温度；`N`、`L`、`R`、`C`分别建立正常坐姿、
左拉伸、右拉伸和胸椎舒展的固定 3000ms 标注窗口，蜂鸣器提示开始与结束。

在本项目的 VS Code WSL 终端运行：

```bash
cd /home/shenicest
Chair/tools/run_capture_com8.sh --guided --participant P01 --session S01 \
  --repetitions 1 --seed 20260829
```

终端会逐项显示动作，等待操作者按 Enter，倒数后自动采集 3 秒。
正式采集时增加 `--repetitions`；CSV 写入 `Chair/data/`，并由该目录的
`.gitignore` 排除，不得提交参与者数据。

### 三个动作分别采集

为了让每个 CSV 只含一种动作标签，正式采集优先使用下面三个独立入口。
它们都只访问 `COM8`，每次动作固定采集 3 秒：

```bash
# 只采集向左拉伸
Chair/tools/capture_left_com8.sh --participant P01 --session LEFT01 --repetitions 10

# 只采集向右拉伸
Chair/tools/capture_right_com8.sh --participant P01 --session RIGHT01 --repetitions 10

# 只采集胸椎舒展
Chair/tools/capture_chest_com8.sh --participant P01 --session CHEST01 --repetitions 10
```

每次准备好后按 Enter，程序倒数 3 秒，然后采集该动作 3 秒；完成后先恢复
正常坐姿，再准备下一次。生成的文件名分别包含 `left_stretch`、
`right_stretch` 或 `chest_extension`，CSV 中的 `collection_mode` 为
`single_action`。输入 `q` 或按 `Ctrl-C` 会保留此前已经完成的窗口。
