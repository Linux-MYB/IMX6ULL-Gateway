#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
plc_sim.py —— 用一对虚拟串口（pty）模拟 PLC 从站，没有真 PLC 也能联调。

用法：
    python3 plc_sim.py            # 打印网关要用的串口路径，例如 /dev/pts/5
    # 另开一个终端：
    GW_SERIAL=/dev/pts/5 ./gateway

模拟的 4 个工艺寄存器：
    reg0 主轴状态（0 停机 / 1 低速 / 2 高速）轮询会自动在 0/1/2 之间切换
    reg1 故障码（一直是 0）
    reg2 产量（每 10 秒 +1）
    reg3 节拍（固定 37）

收到 0x06 写寄存器（停机报文）会打印出来，并把主轴状态清 0。
"""
import os
import sys
import time
import select
import struct

SLAVE = 1


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def build_response(addr, func, payload: bytes) -> bytes:
    body = bytes([addr, func]) + payload
    crc = crc16(body)
    return body + struct.pack("<H", crc)


def main():
    master_fd, slave_fd = os.openpty()
    slave_path = os.ttyname(slave_fd)
    # 关掉行缓冲/回显，按原始串口用
    import tty
    tty.setraw(master_fd)
    tty.setraw(slave_fd)

    regs = [1, 0, 0, 37]
    start_time = time.time()

    print("PLC 模拟器已启动")
    print("网关用的串口路径：", slave_path)
    print("启动网关： GW_SERIAL=%s ./gateway" % slave_path)
    sys.stdout.flush()

    buf = b""
    while True:
        # 主轴状态随运行时间变化：0 → 1 → 2 → 0 ...
        elapsed = int(time.time() - start_time)
        regs[0] = (elapsed // 10) % 3
        regs[2] = elapsed // 10

        r, _, _ = select.select([master_fd], [], [], 0.2)
        if not r:
            continue

        try:
            chunk = os.read(master_fd, 256)
        except OSError:
            break
        if not chunk:
            break

        buf += chunk
        if len(buf) < 8:
            continue

        addr, func = buf[0], buf[1]
        if addr != SLAVE:
            buf = buf[8:]
            continue

        crc_calc = crc16(buf[:6])
        crc_recv = struct.unpack("<H", buf[6:8])[0]
        if crc_calc != crc_recv:
            print("[sim] CRC 错，丢弃")
            buf = buf[8:]
            continue

        if func == 0x03:
            start, count = struct.unpack(">HH", buf[2:6])
            values = b""
            for i in range(count):
                idx = start + i
                values += struct.pack(">H", regs[idx] if idx < len(regs) else 0)
            resp = build_response(addr, func, bytes([count * 2]) + values)
            os.write(master_fd, resp)
        elif func == 0x06:
            reg, val = struct.unpack(">HH", buf[2:6])
            print("[sim] 收到写寄存器 reg%d = %d（停机报文）" % (reg, val))
            if reg < len(regs):
                regs[reg] = val
            if reg == 0x10 and val == 1:
                regs[0] = 0
                print("[sim] 主轴已停机")
            os.write(master_fd, buf[:8])
            sys.stdout.flush()
        else:
            print("[sim] 未支持的功能码 0x%02X" % func)

        buf = buf[8:]


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n模拟器退出")
