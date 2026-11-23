#!/usr/bin/env python3
"""Checks the DSU packet against what Eden validates (udp_protocol.cpp, Validate)."""

import struct
import zlib

import ctl

packet = ctl.dsu_packet(ctl.DSU["circle"])
magic, version, payload_length, crc, _, kind = struct.unpack_from("<IHHIII", packet)
assert (magic, version, kind) == (0x53555344, 1001, 0x00100002)
assert len(packet) == 20 + 80 and payload_length == 80 + 4
assert crc == zlib.crc32(packet[:8] + b"\0\0\0\0" + packet[12:])
assert struct.unpack_from("<H", packet, 20 + 16)[0] == 0x2000
print("ok")
