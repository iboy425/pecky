"""Run and summarize the Qingxian chair hardware self-test on COM8 only."""

from __future__ import annotations

import argparse
import sys
import time

import serial


PORT = "COM8"
BAUD = 115200

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8")

EXPECTED = (
    "HC1_LEFT_OUTER",
    "HC2_LEFT_INNER",
    "HC3_CENTER",
    "HC4_RIGHT_INNER",
    "HC5_RIGHT_OUTER",
    "MPU6050",
    "DHT11",
)

WIRING = {
    "HC1_LEFT_OUTER": (
        "HC1左外：VCC->5V，GND->GND，TRIG->GPIO4；"
        "ECHO->10K->节点，节点->GPIO5，节点->20K->GND。"
    ),
    "HC2_LEFT_INNER": (
        "HC2左内：VCC->5V，GND->GND，TRIG->GPIO6；"
        "ECHO->10K->节点，节点->GPIO7，节点->20K->GND。"
    ),
    "HC3_CENTER": (
        "HC3中央：VCC->5V，GND->GND，TRIG->GPIO10；"
        "ECHO->10K->节点，节点->GPIO11，节点->20K->GND。"
    ),
    "HC4_RIGHT_INNER": (
        "HC4右内：VCC->5V，GND->GND，TRIG->GPIO15；"
        "ECHO->10K->节点，节点->GPIO16，节点->20K->GND。"
    ),
    "HC5_RIGHT_OUTER": (
        "HC5右外：VCC->5V，GND->GND，TRIG->GPIO17；"
        "ECHO->10K->节点，节点->GPIO18，节点->20K->GND。"
    ),
    "MPU6050": (
        "MPU-6050：VCC->3V3，GND->GND，SDA->GPIO8，"
        "SCL->GPIO9，AD0->GND；XDA/XCL/INT暂不接。"
    ),
    "DHT11": "DHT11：+->3V3，-->GND，S/OUT->GPIO12。",
}


def main() -> int:
    parser = argparse.ArgumentParser(
        description="只打开COM8并运行清闲椅硬件自检；不会枚举其他串口。"
    )
    parser.add_argument("--seconds", type=float, default=25.0)
    args = parser.parse_args()

    results: dict[str, tuple[str, str]] = {}
    saw_boot = False
    saw_summary = False
    buzzer_commanded = False

    print(f"只打开 {PORT}，波特率 {BAUD}，监视 {args.seconds:g} 秒")
    try:
        with serial.Serial(
            PORT,
            BAUD,
            timeout=0.25,
            write_timeout=1.0,
            rtscts=False,
            dsrdtr=False,
        ) as device:
            device.dtr = False
            device.rts = False
            time.sleep(0.5)
            device.reset_input_buffer()
            device.write(b"R\n")
            device.flush()

            deadline = time.monotonic() + args.seconds
            while time.monotonic() < deadline:
                raw = device.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                print(line)

                saw_boot |= line.startswith("BOOT,QINGXIAN_CHAIR_")
                saw_summary |= line.startswith("SUMMARY,")
                buzzer_commanded |= line.startswith(
                    "CHECK,BUZZER,MANUAL_CONFIRM"
                )

                if not line.startswith("CHECK,"):
                    continue
                fields = line.split(",", 3)
                if len(fields) < 3:
                    continue
                _, sensor, status = fields[:3]
                detail = fields[3] if len(fields) == 4 else ""
                if sensor in EXPECTED:
                    results[sensor] = (status, detail)

                if saw_summary and all(sensor in results for sensor in EXPECTED):
                    break
    except serial.SerialException as error:
        print(f"ERROR: 无法打开 {PORT}: {error}", file=sys.stderr)
        print(
            "请确认开发板连接的是COM8，并关闭正在占用COM8的串口监视器。",
            file=sys.stderr,
        )
        return 2

    print("\n=== COM8 清闲椅硬件自检结果 ===")
    if not saw_boot:
        print("固件启动标识：未看到（可能尚未烧录本项目固件）")

    failed: list[str] = []
    for sensor in EXPECTED:
        result = results.get(sensor)
        if result is None:
            print(f"{sensor}: 未返回结果")
            failed.append(sensor)
            continue
        status, detail = result
        print(f"{sensor}: {status} {detail}".rstrip())
        if status != "PASS":
            failed.append(sensor)

    print(
        "BUZZER: 已发送测试音，请人工确认是否听到"
        if buzzer_commanded
        else "BUZZER: 未看到测试音命令"
    )

    if failed:
        print("\n=== 失败通道的接线检查 ===")
        for sensor in failed:
            print(f"- {WIRING[sensor]}")

        ultrasonic_failures = [name for name in failed if name.startswith("HC")]
        if len(ultrasonic_failures) == 5:
            print(
                "- 五路超声全部失败时，先检查ESP32 5V是否真的接到五个HC-SR04的VCC，"
                "并确认所有GND共地；测试时在传感器正前方20～50cm放一块硬纸板。"
            )
        elif ultrasonic_failures:
            print(
                "- NO_ECHO也可能是传感器前方没有合适目标；请在失败模块正前方"
                "20～50cm放硬纸板后重跑。"
            )

        print("结论：硬件链路未完全通过。")
        return 1

    if not buzzer_commanded:
        print("结论：传感器通过，但蜂鸣器测试命令未确认。")
        return 1

    print("结论：7路传感器链路均通过；蜂鸣器仍需人工确认声音。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
