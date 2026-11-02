# 机床加工工位边缘安全采集网关

i.MX6ULL 上跑的边缘网关程序：一把接 PLC（RS485/Modbus-RTU），一把接 STM32 检测节点（CAN），
再加温度、烟雾两路环境感知，做两级告警、本地存储、断网补传和 LCD 显示。

代码按“采集 → 判定 → 存储 / 上云 / 显示”拆成 7 个线程，线程之间只走邮箱队列，不共享全局变量。

```
  PLC ──RS485/Modbus-RTU──┐
                          ├─→ judge ─┬─→ store   （SQLite 环形表 + 待上传标记）
  STM32 ──CAN──────┐      │          ├─→ upload  （MQTT 上报 / 阈值订阅）
                   └──────┤          └─→ ui      （framebuffer 显示）
  DS18B20 / MQ-2 ─────────┘
```

## 目录

```
gateway/
├── include/gw.h          运行参数、消息结构、模块接口（先看这个）
├── src/
│   ├── main.c            入口：初始化各模块、注册 7 个线程、judge 判定逻辑
│   ├── mailbox.c         线程间邮箱（环形队列 + 互斥锁 + 条件变量）
│   ├── modbus.c          Modbus-RTU 主站（termios + CRC16 + 超时重传）
│   ├── canbus.c          SocketCAN 收帧与解析
│   ├── sensor.c          DS18B20 温度 + MQ-2 烟雾（IIO）
│   ├── store.c           SQLite 环形表、待上传查询与标记
│   ├── alarm.c           两级判定、阈值管理、蜂鸣器
│   ├── ui.c              LCD 主界面
│   └── upload.c          MQTT 上报与补传（paho，可选编译）
├── lib/                  现成的 framebuffer 与 8x16 字库（从 ds18b20 例程拿来）
├── install/              交叉编译好的 sqlite3（lib + include）
├── tools/plc_sim.py       没有真 PLC 时用的从站模拟器（虚拟串口）
└── Makefile
```

## 编译

```bash
make                 # 交叉编译出 gateway（ARM）
make host            # 在 PC 上编一个版本，用来试逻辑
make MQTT=1 PAHO_INC=/path/to/paho/src PAHO_LIB=/path/to/paho/lib   # 带 MQTT
```

依赖：`arm-linux-gnueabihf-gcc`、`libsqlite3`（工程内已带 ARM 版）。

## 部署到板子

```bash
# 板子上先确认设备和总线
ls /dev/fb0 /dev/beep_misc /dev/ds18b20_misc
ip link set can0 up type can bitrate 500000

# 拷贝程序和数据目录
scp gateway root@<板子IP>:/root/
scp install/lib/libsqlite3.so.0.8.6 root@<板子IP>:/usr/lib/
ssh root@<板子IP> "ln -sf /usr/lib/libsqlite3.so.0.8.6 /usr/lib/libsqlite3.so.0"

# 运行
./gateway
```

运行时可临时改设备路径（不用重新编译）：

```bash
GW_SERIAL=/dev/ttymxc2 GW_CAN=can0 GW_DB=/root/gw.db GW_FB=/dev/fb0 ./gateway
```

## 接线

| 信号 | 板子侧 | 现场侧 | 备注 |
| --- | --- | --- | --- |
| RS485 A/B | UART2 + MAX485 | PLC 的 485 口 | A 接 A、B 接 B，两端共地 |
| CAN_H/CAN_L | CAN 收发器 | STM32 节点 | 总线两端各一个 120Ω 终端电阻 |
| DS18B20 | 单总线 GPIO | 工位测温点 | 数据脚需要 4.7k 上拉 |
| MQ-2 AO | ADC 通道 | 作业区上方 | 上电预热几分钟读数才稳 |
| 蜂鸣器 | GPIO | 现场 | 通过 /dev/beep_misc 控制 |
| LCD | RGB 并口 | — | /dev/fb0，32bpp |

## 没有 PLC 怎么联调

```bash
python3 tools/plc_sim.py          # 打印一个 /dev/pts/N
GW_SERIAL=/dev/pts/N ./gateway    # 另开终端
```

模拟器会周期改变主轴状态，收到 0x06 写寄存器时会打印“停机报文”，可以用来演示一级告警联动停机。

## 演示脚本（答辩/面试用）

1. 启动 `plc_sim.py`，再启动 `gateway`，串口里能看到 PLC 寄存器在变、屏幕上有数值；
2. 用手捏 DS18B20 或吹热风，屏幕温度上升，超过阈值后蜂鸣器响、告警条变红（二级）；
3. 遮住 AI 节点摄像头（或用 `cansend can0 100#01` 手动发一帧），触发一级告警，串口打印“已下发紧急停机报文”，模拟器打印“主轴已停机”；
4. 拔掉网线，数据继续落库；插回网线，upload 线程把期间积压的帧按时间戳补传（串口能看到补传日志）；
5. `sqlite3 /root/gw.db "select count(*), max(ts) from frames;"` 证明数据在本地。

## 和简历条目对照

| 简历里的话 | 在这个工程里对应 |
| --- | --- |
| Modbus-RTU 轮询 4 个工艺寄存器 | `src/modbus.c` `modbus_read_regs()` + `th_modbus` |
| termios 非阻塞串口、CRC16、超时重传 | `modbus_open()` 的 raw 配置、`crc16()`、`tx_and_rx()` 重传 3 次 |
| 解析 3 条 CAN 报文 | `src/canbus.c` `can_read_msg()`，ID 0x100/0x102/0x103 |
| w1-gpio 驱动 DS18B20 | `src/sensor.c`，优先 `/dev/ds18b20_misc`，退化到 w1 sysfs |
| ADC 采样 MQ-2 换算烟雾浓度 | `sensor_read_smoke()`，读 IIO raw × scale |
| 多线程 + 邮箱队列解耦 | `src/mailbox.c` + `main.c` 注册 7 个线程 |
| 两级预警 + Modbus 停机报文 | `alarm.c` `alarm_judge()` + `main.c` 里 `modbus_write_reg(0x10,1)` |
| SQLite 环形表 | `store.c` `ring_trim()` 保留最近 4800 条 |
| 断网先落本地、恢复补传 | `store` 的 `uploaded` 标记 + `upload.c` `flush_pending()` |
| MQTT 心跳保活、阈值下发 | `upload.c` `mqtt_try_connect()` / `parse_threshold()` |
| LVGL 界面 | 目前是 framebuffer + 8x16 字库；`lib/` 里已就位，换 LVGL 是下一步 |

## 已知限制

- **界面只有英文**：8x16 点阵没有中文字库，要中文得换 LVGL + 中文字体；
- **MQTT 默认不编译**：工程里没有 ARM 版 paho，`make MQTT=1` 需要先交叉编译 paho；
- **MQ-2 是估算**：`sensor.c` 里的 `SMOKE_MV_BASE` / `SMOKE_PPM_PER_MV` 是标定系数，按实际传感器改；
- **AI 判决靠 CAN 上报**：检测在 STM32 端，网关只负责接收和联动，不在本工程里。
