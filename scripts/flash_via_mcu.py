#!/usr/bin/env python3

import argparse
import os
import sys
import time

import serial


DEFAULT_CONNECT_BAUDS = [
    115200,
    9600,
    19200,
    38400,
    57600,
    230400,
    256000,
    512000,
    921600,
    4800,
    2400,
]


def connect_screen(ser: serial.Serial, connect_bauds):
    connect_cmd = b"connect\xff\xff\xff"
    for baud in connect_bauds:
        ser.baudrate = baud
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        ser.write(connect_cmd)
        ser.flush()
        time.sleep(max(0.30, (1000000 / baud + 30) / 1000))
        resp = ser.read(256)
        if b"comok" in resp:
            return baud, resp
    return None, b""


def wait_chunk_ack(ser: serial.Serial, timeout_s: float) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        b = ser.read(1)
        if b == b"\x05":
            return True
    return False


def flash_via_mcu(args):
    file_size = os.path.getsize(args.file)
    print(f"PORT={args.port}")
    print(f"FILE={args.file}")
    print(f"SIZE={file_size}")

    with serial.Serial(args.port, 115200, timeout=0.6, write_timeout=2) as ser:
        connected_baud, connect_resp = connect_screen(ser, DEFAULT_CONNECT_BAUDS)
        if connected_baud is None:
            print("CONNECT_FAIL")
            return 1

        print(f"CONNECT_OK baud={connected_baud} resp_hex={connect_resp.hex()}")
        ser.baudrate = connected_baud

        whmi_cmd = f"whmi-wri {file_size},115200,0".encode("ascii") + b"\xff\xff\xff"
        ser.write(whmi_cmd)
        ser.flush()
        print(f"WHMI_CMD_HEX={whmi_cmd.hex()}")

        time.sleep(args.boot_wait_s)
        boot_ack = ser.read(1)
        print(f"BOOT_ACK={boot_ack.hex() if boot_ack else ''}")
        if boot_ack != b"\x05":
            print("BOOT_ACK_FAIL")
            return 2

        sent = 0
        chunks = 0
        t0 = time.time()
        with open(args.file, "rb") as f:
            while True:
                data = f.read(args.chunk_size)
                if not data:
                    break

                ser.write(data)
                ser.flush()
                sent += len(data)
                chunks += 1

                if not wait_chunk_ack(ser, args.ack_timeout_s):
                    print(f"ACK_TIMEOUT chunk={chunks} sent={sent}")
                    return 3

                if chunks <= 3 or chunks % 64 == 0:
                    print(f"CHUNK_OK chunk={chunks} sent={sent}")

        elapsed = time.time() - t0
        speed = (sent / 1024) / elapsed if elapsed > 0 else 0
        print(
            f"FLASH_OK chunks={chunks} sent={sent} elapsed_s={elapsed:.2f} speed_kB_s={speed:.2f}"
        )
        return 0


def parse_args():
    parser = argparse.ArgumentParser(
        description="Flash TJC screen through MCU bridge serial port."
    )
    parser.add_argument("--port", required=True, help="MCU serial port path.")
    parser.add_argument("--file", required=True, help="Path to .tft firmware file.")
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=4096,
        help="Chunk size for data phase (default: 4096).",
    )
    parser.add_argument(
        "--ack-timeout-s",
        type=float,
        default=5.0,
        help="ACK timeout per chunk in seconds (default: 5.0).",
    )
    parser.add_argument(
        "--boot-wait-s",
        type=float,
        default=0.35,
        help="Wait time before reading first 0x05 after whmi-wri (default: 0.35).",
    )
    return parser.parse_args()


if __name__ == "__main__":
    cli_args = parse_args()
    if not os.path.isfile(cli_args.file):
        print(f"FILE_NOT_FOUND {cli_args.file}")
        sys.exit(10)
    if cli_args.chunk_size <= 0:
        print("INVALID_CHUNK_SIZE")
        sys.exit(11)
    sys.exit(flash_via_mcu(cli_args))
