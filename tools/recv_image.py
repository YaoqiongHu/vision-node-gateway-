#!/usr/bin/env python3
"""从串口接收一帧 RGB565 图像并存成 PNG。

固件那边（node/app/main.c 的 camera_selftest）发的格式：

    'I' 'M' 'G' '!'    4 字节魔数
    width   uint16 LE
    height  uint16 LE
    bytes   uint32 LE   紧跟其后的像素字节数（= width * height * 2）
    payload             RGB565，每像素 2 字节，**高字节在前**

用法：
    python3 tools/recv_image.py                       # 自动找串口
    python3 tools/recv_image.py -p /dev/ttyUSB0       # 指定串口
    python3 tools/recv_image.py -o out.png --baud 921600

依赖：pyserial（`pip install pyserial`）。没有 pyserial 会用 stty + 裸读兜底。
只用标准库生成 PNG，不需要 Pillow。
"""

import argparse
import glob
import os
import struct
import sys
import time
import zlib

MAGIC = b"IMG!"


# --------------------------------------------------------------------------- PNG
def write_png(path, width, height, rgb_rows):
    """把 RGB888 的字节串写成 PNG（每行 3*width 字节）。"""

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    raw = bytearray()
    for y in range(height):
        raw.append(0)  # 每行的 filter type：0 = None
        raw += rgb_rows[y * width * 3:(y + 1) * width * 3]

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8bit RGB
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", ihdr)
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def rgb565_to_rgb888(data):
    """RGB565（高字节在前）→ RGB888。5/6/5 位左移补到 8 位。"""
    out = bytearray(len(data) // 2 * 3)
    j = 0
    for i in range(0, len(data) - 1, 2):
        px = (data[i] << 8) | data[i + 1]
        r = (px >> 11) & 0x1F
        g = (px >> 5) & 0x3F
        b = px & 0x1F
        out[j] = (r << 3) | (r >> 2)
        out[j + 1] = (g << 2) | (g >> 4)
        out[j + 2] = (b << 3) | (b >> 2)
        j += 3
    return bytes(out)


# ------------------------------------------------------------------------ 串口
def pick_port(explicit):
    if explicit:
        return explicit
    for pattern in ("/dev/ttyUSB*", "/dev/ttyACM*", "/dev/ttyCH341USB*"):
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    return None


def open_serial(port, baud, timeout=1.0):
    try:
        import serial  # noqa: WPS433
    except ImportError:
        return None

    return serial.Serial(port, baud, timeout=timeout)


def read_exact_serial(ser, n, timeout):
    end = time.time() + timeout
    buf = bytearray()
    while len(buf) < n and time.time() < end:
        chunk = ser.read(n - len(buf))
        if chunk:
            buf += chunk
        else:
            time.sleep(0.002)
    return bytes(buf)


def read_exact_fd(fd, n, timeout):
    end = time.time() + timeout
    buf = bytearray()
    while len(buf) < n and time.time() < end:
        try:
            chunk = os.read(fd, n - len(buf))
        except BlockingIOError:
            time.sleep(0.002)
            continue
        if not chunk:
            time.sleep(0.002)
            continue
        buf += chunk
    return bytes(buf)


def sync_magic(read_one, timeout):
    """一个字节一个字节读，直到拼出魔数。"""
    window = bytearray()
    end = time.time() + timeout
    while time.time() < end:
        b = read_one()
        if not b:
            continue
        window.append(b[0])
        if len(window) > len(MAGIC):
            del window[0]
        if bytes(window) == MAGIC:
            return True
    return False


# -------------------------------------------------------------------------- 主
def main():
    ap = argparse.ArgumentParser(description="从串口收一帧 RGB565 并存成 PNG")
    ap.add_argument("-p", "--port", help="串口设备，默认自动找 /dev/ttyUSB* 或 /dev/ttyACM*")
    ap.add_argument("-b", "--baud", type=int, default=115200, help="波特率，默认 115200")
    ap.add_argument("-o", "--out", default="frame.png", help="输出文件名，默认 frame.png")
    ap.add_argument("-t", "--timeout", type=float, default=10.0, help="等待魔数的秒数")
    args = ap.parse_args()

    port = pick_port(args.port)
    if not port:
        sys.exit("找不到串口。用 -p /dev/ttyUSB0 指定。"
                 "\n（Linux 上如果不是 /dev/ttyUSB*，用 `ls /dev/tty*` 找）")

    print(f"[recv] port={port} baud={args.baud}")
    print("[recv] 复位板子（或按 EN 键）让固件重新发一帧……")

    ser = open_serial(port, args.baud)
    fd = None
    if ser is None:
        print("[recv] 没有 pyserial，改用 stty + 裸读")
        os.system(f"stty -F {port} {args.baud} raw -echo -hupcl")
        fd = os.open(port, os.O_RDONLY | os.O_NONBLOCK)

    def read_one():
        if ser is not None:
            return ser.read(1)
        return read_exact_fd(fd, 1, 0.1)

    try:
        if not sync_magic(read_one, args.timeout):
            sys.exit("[recv] 超时：没等到图像头。检查波特率、接线（TX/RX 是否交叉）、"
                     "以及固件里 [cam] capture OK 有没有打印。")

        hdr = read_exact_serial(ser, 8, 5.0) if ser is not None else read_exact_fd(fd, 8, 5.0)
        if len(hdr) != 8:
            sys.exit("[recv] 头只收到 %d 字节" % len(hdr))

        width, height, nbytes = struct.unpack("<HH I", hdr)
        expect = width * height * 2
        if nbytes != expect:
            print(f"[recv] 警告：头里写 {nbytes} 字节，按 {width}x{height}x2 应为 {expect}")

        print(f"[recv] 收到头：{width}x{height}，{nbytes} 字节，开始收像素……")
        t0 = time.time()
        payload = (read_exact_serial(ser, nbytes, 30.0) if ser is not None
                   else read_exact_fd(fd, nbytes, 30.0))
        dt = time.time() - t0
    finally:
        if ser is not None:
            ser.close()
        if fd is not None:
            os.close(fd)

    if len(payload) != nbytes:
        sys.exit(f"[recv] 像素只收到 {len(payload)}/{nbytes} 字节，传输中断")

    print(f"[recv] 收完，用时 {dt:.2f} s（{nbytes / max(dt, 1e-6) / 1024:.1f} KB/s）")

    uniq = len(set(payload[:4096]))
    if uniq <= 2:
        print("[recv] 警告：前 4 KB 只有 %d 种字节，画面很可能是纯色（没抓到有效数据）" % uniq)

    rgb = rgb565_to_rgb888(payload)
    write_png(args.out, width, height, rgb)
    print(f"[recv] 已保存 {args.out}（{width}x{height}）")


if __name__ == "__main__":
    main()
