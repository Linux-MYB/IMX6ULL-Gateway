# 机床工位边缘安全采集网关

i.MX6ULL 上的边缘网关。RS485/Modbus-RTU 采集 PLC 工艺数据，CAN 接收 STM32 检测节点上报，
DS18B20 与 MQ-2 做环境感知，实现两级告警、本地存储、断网补传和 LCD 显示。

## 结构

```
include/gw.h       参数与接口
src/               main mailbox modbus canbus sensor store alarm ui upload
lib/               framebuffer 与 8x16 字库
install/           ARM 版 sqlite3
tools/plc_sim.py   无 PLC 时用的从站模拟器
```

## 编译运行

```bash
make
GW_SERIAL=/dev/ttymxc2 GW_CAN=can0 ./gateway
```

启用 MQTT：`make MQTT=1 PAHO_INC=... PAHO_LIB=...`

没有 PLC 时：

```bash
python3 tools/plc_sim.py
GW_SERIAL=/dev/pts/N ./gateway
```
