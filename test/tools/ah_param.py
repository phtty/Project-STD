#!/usr/bin/env python3
"""AH/TCP 参数口命令行：0AH 设置 / 0BH 采集。

帧格式（《IP 化设备通讯协议》§1.1）::

    FF FF | VER(00) | SEQ | LEN(4, 大端) | DATA | CRC16

CRC 为 CRC-16/X25（覆盖 VER..DATA）。0AH 请求固定 84 字节 + 模块表，
0AH 应答 21 字节（ErrCode 在 DATA[20]）；0BH 请求 20 字节，
0BH 应答 84 字节 + 模块表（本机模块类型/序号，31H 多 1 字节 FontLine）。

用法::

    # 读参数（默认端口 6001）
    test/tools/ah_param.py get --host 192.168.1.50

    # 写参数：只下发给出的字段，其余 0 = 保持原值；设备模块 32H/序号 01
    test/tools/ah_param.py set --host 192.168.1.50 \\
        --lane 5201131401 --mqtt-ip 120.46.136.199 --mqtt-port 6000 \\
        --mqtt-user pxh --mqtt-pwd secret --device 32 --device-no 01

    # 不发网络请求，只验证组帧/CRC/解析
    test/tools/ah_param.py self-test

注意：0AH 只做“校验 → 落盘 → 应答”，网络/MQTT/主题都**重启后**才生效；
应答 ErrCode=0 只表示已落盘，不代表运行态已切换。
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time

STX = b"\xff\xff"
VER = 0x00
CMD_SET = 0x0A
CMD_GET = 0x0B

FIXED_LEN = 84
REQ_GET_LEN = 20
RSP_SET_LEN = 21

OFF_CMD = 0
OFF_UNIX_TM = 1
OFF_LANE = 5
OFF_CERT = 10
OFF_SERVER1_IP = 20
OFF_SERVER1_PORT = 24
OFF_SERVER2_IP = 26
OFF_SERVER2_PORT = 30
OFF_SERVER3_IP = 32
OFF_SERVER3_PORT = 36
OFF_MQTT_IP = 38
OFF_MQTT_PORT = 42
OFF_MQTT_USER = 44
OFF_MQTT_PWD = 54
OFF_MQTT_QOS = 64
OFF_NTP_IP = 65
OFF_CLIENT_IP = 69
OFF_CLIENT_PORT = 73
OFF_GATEWAY = 75
OFF_NETMASK = 79
OFF_MODE_CNT = 83
OFF_MODULES = 84

ERR_NAMES = {
    0x00: "OK",
    0x01: "PARAM",
    0x02: "SAVE",
    0x03: "NOT_READY",
    0x04: "CMD",
    0x05: "FORMAT",
}

MODULE_SIZE = {0x31: 3, 0x32: 2}


def crc16_x25(data: bytes) -> int:
    """CRC-16/X25：poly 0x1021（反射 0x8408）、init 0xFFFF、xorout 0xFFFF。"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc ^ 0xFFFF


def build_frame(seq: int, data: bytes) -> bytes:
    if not 0 <= seq <= 0xFF:
        raise ValueError("SEQ 必须是 0..255")
    if len(data) > 128:
        raise ValueError("DATA 超过 128 字节")
    header = STX + bytes([VER, seq]) + struct.pack(">I", len(data))
    body = header[2:] + data
    return header + data + struct.pack(">H", crc16_x25(body))


def build_get(seq: int = 0x01, unix_tm: int | None = None) -> bytes:
    ts = int(time.time()) if unix_tm is None else unix_tm
    data = bytearray(REQ_GET_LEN)
    data[OFF_CMD] = CMD_GET
    data[OFF_UNIX_TM : OFF_UNIX_TM + 4] = struct.pack(">I", ts & 0xFFFFFFFF)
    return build_frame(seq, bytes(data))


def build_set(args: argparse.Namespace) -> bytes:
    data = bytearray(FIXED_LEN + 3)
    data[OFF_CMD] = CMD_SET

    def put_ip(off: int, value: str | None) -> None:
        if value:
            data[off : off + 4] = socket.inet_aton(value)

    def put_port(off: int, value: int | None) -> None:
        if value is not None:
            data[off : off + 2] = struct.pack(">H", value & 0xFFFF)

    def put_str(off: int, value: str | None, width: int) -> None:
        if value:
            raw = value.encode("utf-8")
            if len(raw) > width:
                raise ValueError(f"字段最长 {width} 字节：{value!r}")
            data[off : off + len(raw)] = raw

    if args.unix_tm is not None:
        data[OFF_UNIX_TM : OFF_UNIX_TM + 4] = struct.pack(">I", args.unix_tm & 0xFFFFFFFF)
    if args.lane:
        lane = bytes.fromhex(args.lane.replace(":", "").replace(" ", ""))
        if len(lane) != 5:
            raise ValueError("--lane 必须是 10 个 hex 字符（站 4 B + 车道 1 B）")
        data[OFF_LANE : OFF_LANE + 5] = lane
    if args.cert:
        cert = bytes.fromhex(args.cert.replace(":", "").replace(" ", ""))
        if len(cert) != 8:
            raise ValueError("--cert 必须是 16 个 hex 字符")
        data[OFF_CERT : OFF_CERT + 8] = cert

    put_ip(OFF_SERVER1_IP, args.server1_ip)
    put_port(OFF_SERVER1_PORT, args.server1_port)
    put_ip(OFF_SERVER2_IP, args.server2_ip)
    put_port(OFF_SERVER2_PORT, args.server2_port)
    put_ip(OFF_SERVER3_IP, args.server3_ip)
    put_port(OFF_SERVER3_PORT, args.server3_port)

    put_ip(OFF_MQTT_IP, args.mqtt_ip)
    put_port(OFF_MQTT_PORT, args.mqtt_port)
    put_str(OFF_MQTT_USER, args.mqtt_user, 10)
    put_str(OFF_MQTT_PWD, args.mqtt_pwd, 10)
    if args.mqtt_qos is not None:
        data[OFF_MQTT_QOS] = args.mqtt_qos & 0xFF

    put_ip(OFF_NTP_IP, args.ntp_ip)
    put_ip(OFF_CLIENT_IP, args.client_ip)
    put_port(OFF_CLIENT_PORT, args.client_port)
    put_ip(OFF_GATEWAY, args.gateway)
    put_ip(OFF_NETMASK, args.netmask)

    if args.device is None:
        data[OFF_MODE_CNT] = 0
        return build_frame(args.seq, bytes(data[:FIXED_LEN]))

    if args.device not in MODULE_SIZE:
        raise ValueError("本期只支持模块类型 0x31/0x32")
    size = MODULE_SIZE[args.device]
    data[OFF_MODE_CNT] = 1
    data[OFF_MODULES] = args.device
    data[OFF_MODULES + 1] = (args.device_no or 0) & 0xFF
    if args.device == 0x31:
        data[OFF_MODULES + 2] = (args.font_line or 0) & 0xFF
    return build_frame(args.seq, bytes(data[: FIXED_LEN + size]))


def recv_exact(sock: socket.socket, count: int) -> bytes:
    buf = bytearray()
    while len(buf) < count:
        chunk = sock.recv(count - len(buf))
        if not chunk:
            raise ConnectionError(f"连接被关闭（还差 {count - len(buf)} 字节）")
        buf.extend(chunk)
    return bytes(buf)


def recv_frame(sock: socket.socket) -> tuple[int, bytes]:
    header = recv_exact(sock, 8)
    if header[:2] != STX:
        raise ValueError(f"帧头不是 FF FF：{header.hex()}")
    if header[2] != VER:
        raise ValueError(f"协议版本不是 0x00：{header[2]:02X}")
    if header[4] != 0 or header[5] != 0:
        raise ValueError(f"LEN 高 2 字节非 0：{header.hex()}")
    length = struct.unpack(">H", header[6:8])[0]
    if length == 0 or length > 128:
        raise ValueError(f"LEN 非法：{length}")
    rest = recv_exact(sock, length + 2)
    data, crc_raw = rest[:length], rest[length:]
    crc = struct.unpack(">H", crc_raw)[0]
    want = crc16_x25(header[2:] + data)
    if crc != want:
        raise ValueError(f"CRC 错：收到 {crc:04X}，计算 {want:04X}")
    return header[3], data


def ip_str(raw: bytes) -> str:
    return socket.inet_ntoa(raw)


def cstr(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("utf-8", "replace")


def module_info(device_id: int, module: bytes) -> list[dict]:
    modules: list[dict] = []
    pos = 0
    while pos + 2 <= len(module):
        dev, no = module[pos], module[pos + 1]
        size = MODULE_SIZE.get(dev, 2)
        if pos + size > len(module):
            break
        item = {"device": f"{dev:02X}H", "device_no": f"{no:02X}"}
        if dev == 0x31 and size == 3:
            item["font_line"] = module[pos + 2]
        modules.append(item)
        pos += size
    return modules


def parse_config(data: bytes) -> dict:
    if len(data) < FIXED_LEN:
        raise ValueError(f"应答不足 {FIXED_LEN} 字节：{len(data)}")
    mode_cnt = data[OFF_MODE_CNT]
    modules = data[OFF_MODULES:] if mode_cnt else b""
    return {
        "cmd": f"{data[OFF_CMD]:02X}H",
        "unix_tm": struct.unpack(">I", data[OFF_UNIX_TM : OFF_UNIX_TM + 4])[0],
        "lane_hex": data[OFF_LANE : OFF_LANE + 5].hex().upper(),
        "cert_hex": data[OFF_CERT : OFF_CERT + 8].hex().upper(),
        "server1": [ip_str(data[OFF_SERVER1_IP : OFF_SERVER1_IP + 4]),
                    struct.unpack(">H", data[OFF_SERVER1_PORT : OFF_SERVER1_PORT + 2])[0]],
        "server2": [ip_str(data[OFF_SERVER2_IP : OFF_SERVER2_IP + 4]),
                    struct.unpack(">H", data[OFF_SERVER2_PORT : OFF_SERVER2_PORT + 2])[0]],
        "server3": [ip_str(data[OFF_SERVER3_IP : OFF_SERVER3_IP + 4]),
                    struct.unpack(">H", data[OFF_SERVER3_PORT : OFF_SERVER3_PORT + 2])[0]],
        "mqtt_ip": ip_str(data[OFF_MQTT_IP : OFF_MQTT_IP + 4]),
        "mqtt_port": struct.unpack(">H", data[OFF_MQTT_PORT : OFF_MQTT_PORT + 2])[0],
        "mqtt_user": cstr(data[OFF_MQTT_USER : OFF_MQTT_USER + 10]),
        "mqtt_pwd": cstr(data[OFF_MQTT_PWD : OFF_MQTT_PWD + 10]),
        "mqtt_qos": data[OFF_MQTT_QOS],
        "ntp_ip": ip_str(data[OFF_NTP_IP : OFF_NTP_IP + 4]),
        "client_ip": ip_str(data[OFF_CLIENT_IP : OFF_CLIENT_IP + 4]),
        "client_port": struct.unpack(">H", data[OFF_CLIENT_PORT : OFF_CLIENT_PORT + 2])[0],
        "gateway": ip_str(data[OFF_GATEWAY : OFF_GATEWAY + 4]),
        "netmask": ip_str(data[OFF_NETMASK : OFF_NETMASK + 4]),
        "mode_cnt": mode_cnt,
        "modules": module_info(data[OFF_MODULES], modules) if modules else [],
    }


def transact(host: str, port: int, timeout: float, payload: bytes) -> tuple[int, bytes]:
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(payload)
        return recv_frame(sock)


def cmd_get(args: argparse.Namespace) -> int:
    seq, data = transact(args.host, args.port, args.timeout, build_get(args.seq, args.unix_tm))
    if not data or data[OFF_CMD] != CMD_GET:
        raise ValueError(f"应答命令码不是 0BH：{data[:1].hex()}")
    info = parse_config(data)
    info["seq"] = seq
    print(json.dumps(info, ensure_ascii=False, indent=2))
    return 0


def cmd_set(args: argparse.Namespace) -> int:
    seq, data = transact(args.host, args.port, args.timeout, build_set(args))
    if len(data) != RSP_SET_LEN or data[OFF_CMD] != CMD_SET:
        raise ValueError(f"0AH 应答格式不对（{len(data)} 字节，cmd={data[:1].hex()}）")
    err = data[20]
    print(json.dumps({
        "seq": seq,
        "cmd": f"{data[OFF_CMD]:02X}H",
        "unix_tm": struct.unpack(">I", data[OFF_UNIX_TM : OFF_UNIX_TM + 4])[0],
        "lane_hex": data[OFF_LANE : OFF_LANE + 5].hex().upper(),
        "cert_hex": data[OFF_CERT : OFF_CERT + 8].hex().upper(),
        "err_code": f"{err:02X}",
        "err_name": ERR_NAMES.get(err, "UNKNOWN"),
        "note": "已落盘；网络/MQTT/主题重启后生效",
    }, ensure_ascii=False, indent=2))
    return 0 if err == 0 else 1


def cmd_self_test(_args: argparse.Namespace) -> int:
    failures = 0

    def check(cond: bool, what: str) -> None:
        nonlocal failures
        if cond:
            print(f"  ok   {what}")
        else:
            failures += 1
            print(f"  FAIL {what}")

    check(crc16_x25(b"123456789") == 0x906E, "CRC-16/X25 标准向量 123456789 → 906E")

    get_frame = build_get(0x01, 1741183200)
    check(len(get_frame) == 30, "0BH 请求帧长 30")
    check(get_frame[:4] == b"\xff\xff\x00\x01", "帧头 FF FF 00 01")
    check(get_frame[4:8] == b"\x00\x00\x00\x14", "LEN=20 大端")
    check(struct.unpack(">H", get_frame[-2:])[0] == crc16_x25(get_frame[2:-2]), "0BH CRC 自洽")

    args = argparse.Namespace(
        seq=0x01, unix_tm=1741183200, lane="5201131401", cert=None,
        server1_ip=None, server1_port=None, server2_ip=None, server2_port=None,
        server3_ip=None, server3_port=None, mqtt_ip="120.46.136.199", mqtt_port=6000,
        mqtt_user="pxh", mqtt_pwd="secret", mqtt_qos=None, ntp_ip=None,
        client_ip=None, client_port=None, gateway=None, netmask=None,
        device=0x32, device_no=0x01, font_line=None,
    )
    set_frame = build_set(args)
    check(len(set_frame) == 8 + 86 + 2, "0AH 32H 请求帧长 96")
    check(set_frame[8 + OFF_CMD] == CMD_SET, "0AH CmdType=0A")
    check(set_frame[8 + OFF_LANE : 8 + OFF_LANE + 5] == bytes.fromhex("5201131401"), "LaneHex 落位")
    check(set_frame[8 + OFF_MQTT_IP : 8 + OFF_MQTT_IP + 4] == socket.inet_aton("120.46.136.199"),
          "MQTTIP 落位")
    check(set_frame[8 + OFF_MQTT_PORT : 8 + OFF_MQTT_PORT + 2] == b"\x17\x70", "MQTTPort 6000")
    check(set_frame[8 + OFF_MODE_CNT] == 1 and set_frame[8 + OFF_MODULES] == 0x32,
          "模块表 ModeCnt=1 / 32H")

    # 用 0AH 请求的数据段模拟 0BH 应答公共块，验证解析器
    fake_rsp = bytearray(set_frame[8 : 8 + FIXED_LEN])
    fake_rsp[OFF_CMD] = CMD_GET
    fake_rsp[OFF_CLIENT_IP : OFF_CLIENT_IP + 4] = socket.inet_aton("192.168.1.50")
    fake_rsp[OFF_CLIENT_PORT : OFF_CLIENT_PORT + 2] = struct.pack(">H", 6001)
    fake_rsp[OFF_MODE_CNT] = 1
    parsed = parse_config(bytes(fake_rsp) + bytes([0x32, 0x01]))
    check(parsed["lane_hex"] == "5201131401", "解析 LaneHex")
    check(parsed["mqtt_user"] == "pxh", "解析 MQTTUser")
    check(parsed["client_ip"] == "192.168.1.50", "解析 ClientIP")
    check(parsed["modules"] == [{"device": "32H", "device_no": "01"}], "解析模块表")

    # 坏 CRC 必须被识别
    bad = bytearray(set_frame)
    bad[-1] ^= 0xFF
    try:
        crc = struct.unpack(">H", bytes(bad[-2:]))[0]
        check(crc != crc16_x25(bytes(bad[2:-2])), "坏 CRC 可被检测")
    except Exception:
        check(False, "坏 CRC 可被检测")

    print(f"\n自检：{'通过' if failures == 0 else f'{failures} 项失败'}")
    return 0 if failures == 0 else 1


def add_set_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--seq", type=lambda v: int(v, 0), default=0x01)
    parser.add_argument("--unix-tm", type=int, default=None, help="Unix 时间；写了才校时")
    parser.add_argument("--lane", help="10 个 hex 字符：站 4 B + 车道 1 B")
    parser.add_argument("--cert", help="16 个 hex 字符（只存不校验）")
    parser.add_argument("--server1-ip")
    parser.add_argument("--server1-port", type=int)
    parser.add_argument("--server2-ip")
    parser.add_argument("--server2-port", type=int)
    parser.add_argument("--server3-ip")
    parser.add_argument("--server3-port", type=int)
    parser.add_argument("--mqtt-ip")
    parser.add_argument("--mqtt-port", type=int)
    parser.add_argument("--mqtt-user", help="最多 10 字节")
    parser.add_argument("--mqtt-pwd", help="最多 10 字节")
    parser.add_argument("--mqtt-qos", type=lambda v: int(v, 0))
    parser.add_argument("--ntp-ip")
    parser.add_argument("--client-ip", help="设备 IP")
    parser.add_argument("--client-port", type=int, help="参数口监听端口（重启生效）")
    parser.add_argument("--gateway")
    parser.add_argument("--netmask")
    parser.add_argument("--device", type=lambda v: int(v, 0),
                        help="模块类型：0x31/0x32；不给则 ModeCnt=0（不改模块）")
    parser.add_argument("--device-no", type=lambda v: int(v, 0), help="模块序号 0x01..")
    parser.add_argument("--font-line", type=lambda v: int(v, 0), help="仅 31H：默认行数 0..6")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="AH/TCP 参数口工具（0AH/0BH）")
    sub = parser.add_subparsers(dest="command", required=True)

    p_get = sub.add_parser("get", help="0BH 读取参数")
    p_get.add_argument("--host", required=True)
    p_get.add_argument("--port", type=int, default=6001)
    p_get.add_argument("--timeout", type=float, default=3.0)
    p_get.add_argument("--seq", type=lambda v: int(v, 0), default=0x01)
    p_get.add_argument("--unix-tm", type=int, default=None)
    p_get.set_defaults(func=cmd_get)

    p_set = sub.add_parser("set", help="0AH 写参数（只写给出的字段）")
    p_set.add_argument("--host", required=True)
    p_set.add_argument("--port", type=int, default=6001)
    p_set.add_argument("--timeout", type=float, default=3.0)
    add_set_args(p_set)
    p_set.set_defaults(func=cmd_set)

    p_self = sub.add_parser("self-test", help="离线自检：CRC/组帧/解析")
    p_self.set_defaults(func=cmd_self_test, host=None, port=None, timeout=3.0)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except (OSError, ValueError, struct.error) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
