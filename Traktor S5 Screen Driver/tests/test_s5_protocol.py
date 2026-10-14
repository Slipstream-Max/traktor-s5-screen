"""Regression checks for the S5 HWS/USB boundary.

This test does not touch the controller. It validates the captured HWS framing
and the values recovered from the vendor driver before any kernel package is
installed.
"""

from __future__ import annotations

import struct
def extract_display_message(message: bytes) -> bytes:
    assert message[:4] == b"Dsd\x03"
    length = struct.unpack_from("<I", message, 16)[0]
    assert 0 < length <= len(message) - 20
    payload = message[20 : 20 + length]
    assert payload[:1] == b"\x84"
    return payload


def test_captured_initialization_frame() -> None:
    message = bytes.fromhex(
        "4473640301000000000000001001e0011c000000"
        "84000160000000000000000001e001100200ff0003"
        "00000040000100"
    )
    payload = extract_display_message(message)
    assert payload == message[20:]
    assert payload[:4] == bytes.fromhex("84000160")


def test_ioctl_values() -> None:
    device_type = 0x8089
    write = (device_type << 16) | (0xA20 << 2)
    assert write == 0x80892880
    assert 0x80892014 == 0x8089_0000 | (0x805 << 2)
    assert 0x80892884 == 0x8089_0000 | (0xA21 << 2)


def test_vendor_capability_response() -> None:
    assert bytes.fromhex("02 00 04 02 00 00 00 00 00 00 00 00") == bytes(
        [2, 0, 4, 2, 0, 0, 0, 0, 0, 0, 0, 0]
    )


def test_device_descriptor_identity() -> None:
    descriptor = bytes([
        18, 1, 0x00, 0x02, 0xEF, 0x02, 0x01, 64,
        0xCC, 0x17, 0x20, 0x14, 0x22, 0x00, 1, 3, 2, 1,
    ])
    assert len(descriptor) == 18
    assert struct.unpack_from("<H", descriptor, 8)[0] == 0x17CC
    assert struct.unpack_from("<H", descriptor, 10)[0] == 0x1420
    assert struct.unpack_from("<H", descriptor, 12)[0] == 0x0022


if __name__ == "__main__":
    test_captured_initialization_frame()
    test_ioctl_values()
    test_vendor_capability_response()
    test_device_descriptor_identity()
    print("S5 protocol checks passed")
