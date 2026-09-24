#!/usr/bin/env python3
"""
Build a signed Nordic secure DFU package (.zip) without nrfutil.

Only needs python3 and the `cryptography` package, which makes it usable on
platforms where nrfutil is a pain to install (Apple Silicon in particular).
The output is byte compatible with:

    nrfutil pkg generate --application app.hex --application-version 99 \
        --hw-version 52 --sd-req 0x0103 --key-file priv.pem out.zip

Usage:
    python3 tools/mkdfu.py build/PixlAnalyzerOLED.bin build/PixlAnalyzerOLED.zip
"""

import argparse
import hashlib
import json
import os
import zipfile

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec, utils
from cryptography.hazmat.primitives.serialization import load_pem_private_key

# ---------------------------------------------------------------------------
# Minimal protobuf writer for the handful of fields of dfu-cc.proto we need
# ---------------------------------------------------------------------------


def varint(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        out.append(byte | (0x80 if value else 0))
        if not value:
            return bytes(out)


def tag(field, wire_type):
    return varint((field << 3) | wire_type)


def field_varint(field, value):
    return tag(field, 0) + varint(value)


def field_bytes(field, value):
    return tag(field, 2) + varint(len(value)) + value


def init_command(fw_version, hw_version, sd_req, app_size, fw_hash):
    """InitCommand for a plain application update."""
    msg = b""
    msg += field_varint(1, fw_version)   # fw_version
    msg += field_varint(2, hw_version)   # hw_version
    msg += field_bytes(3, b"".join(varint(s) for s in sd_req))  # sd_req (packed)
    msg += field_varint(4, 0)            # type = APPLICATION
    msg += field_varint(5, 0)            # sd_size
    msg += field_varint(6, 0)            # bl_size
    msg += field_varint(7, app_size)     # app_size
    # hash: the bootloader compares the digest in reversed byte order
    hash_msg = field_varint(1, 3) + field_bytes(2, fw_hash[::-1])  # SHA256
    msg += field_bytes(8, hash_msg)
    msg += field_varint(9, 0)            # is_debug = false
    # boot_validation: VALIDATE_GENERATED_CRC, no extra bytes
    msg += field_bytes(10, field_varint(1, 1) + field_bytes(2, b""))
    return msg


def sign_init(init_msg, key):
    """ECDSA P-256 over SHA256(InitCommand), r and s stored little endian."""
    digest = hashlib.sha256(init_msg).digest()
    der = key.sign(digest, ec.ECDSA(utils.Prehashed(hashes.SHA256())))
    r, s = utils.decode_dss_signature(der)
    return r.to_bytes(32, "big")[::-1] + s.to_bytes(32, "big")[::-1]


def build_init_packet(firmware, fw_version, hw_version, sd_req, key):
    init_msg = init_command(fw_version, hw_version, sd_req, len(firmware),
                            hashlib.sha256(firmware).digest())
    command = field_varint(1, 1) + field_bytes(2, init_msg)  # op_code = INIT
    signed = field_bytes(1, command)
    signed += field_varint(2, 0)                 # ECDSA_P256_SHA256
    signed += field_bytes(3, sign_init(init_msg, key))
    return field_bytes(2, signed)                # Packet.signed_command


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("firmware", help="application binary (build/<target>.bin)")
    ap.add_argument("package", help="output .zip")
    ap.add_argument("--key", default=os.path.join(here, os.pardir, "priv.pem"),
                    help="signing key, default firmware/priv.pem")
    ap.add_argument("--app-version", type=int, default=99)
    ap.add_argument("--hw-version", type=int, default=52)
    ap.add_argument("--sd-req", default="0x0103",
                    help="comma separated SoftDevice FWIDs, default 0x0103 (S112 7.2.0)")
    args = ap.parse_args()

    firmware = open(args.firmware, "rb").read()
    key = load_pem_private_key(open(args.key, "rb").read(), password=None)
    sd_req = [int(v, 0) for v in args.sd_req.split(",")]

    packet = build_init_packet(firmware, args.app_version, args.hw_version, sd_req, key)

    name = os.path.splitext(os.path.basename(args.package))[0]
    manifest = {"manifest": {"application": {"bin_file": name + ".bin",
                                             "dat_file": name + ".dat"}}}
    with zipfile.ZipFile(args.package, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=4))
        z.writestr(name + ".bin", firmware)
        z.writestr(name + ".dat", packet)

    print(f"{args.package}: {len(firmware)} bytes of firmware, "
          f"app-version {args.app_version}, hw-version {args.hw_version}, "
          f"sd-req {', '.join(hex(s) for s in sd_req)}")


if __name__ == "__main__":
    main()
