#!/usr/bin/env python3
"""
生成 InstaCare Peripheral 测试证书链。

用法:
    python generate_certs.py --device-id <32位hex> [--output main/cert_chain.c]

生成文件:
    - cert_chain.c  : 嵌入式 C 数组，包含证书链二进制
    - device_key.pem: 设备 ECDSA P-256 私钥（仅用于调试，生产环境不暴露）
    - device_cert.pem: 设备证书 PEM
    - root_cert.pem : 自签名根证书 PEM（供 Central 测试使用）

依赖:
    pip install cryptography
"""

import argparse
import datetime
import os
import sys

from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.backends import default_backend


def generate_root_ca():
    """生成自签名根 CA（InstaCare 测试用）。"""
    key = ec.generate_private_key(ec.SECP256R1(), default_backend())

    subject = issuer = x509.Name([
        x509.NameAttribute(NameOID.COUNTRY_NAME, "CN"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "InstaCare Test Root"),
        x509.NameAttribute(NameOID.COMMON_NAME, "InstaCare Device Root CA"),
    ])

    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.datetime.now(datetime.UTC))
        .not_valid_after(datetime.datetime.now(datetime.UTC) + datetime.timedelta(days=3650))
        .add_extension(
            x509.BasicConstraints(ca=True, path_length=None),
            critical=True,
        )
        .add_extension(
            x509.KeyUsage(
                digital_signature=True,
                key_cert_sign=True,
                crl_sign=True,
                content_commitment=False,
                key_encipherment=False,
                data_encipherment=False,
                key_agreement=False,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(key.public_key()),
            critical=False,
        )
        .sign(key, hashes.SHA256(), default_backend())
    )

    return key, cert


def generate_device_cert(root_key, root_cert, device_id_hex):
    """签发设备证书，SAN 包含 urn:instacare:device:<device_id>。"""
    device_key = ec.generate_private_key(ec.SECP256R1(), default_backend())

    subject = x509.Name([
        x509.NameAttribute(NameOID.COUNTRY_NAME, "CN"),
        x509.NameAttribute(NameOID.ORGANIZATION_NAME, "InstaCare Test Device"),
        x509.NameAttribute(NameOID.COMMON_NAME, f"InstaCare Device {device_id_hex[:8]}"),
    ])

    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(root_cert.subject)
        .public_key(device_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(datetime.datetime.now(datetime.UTC))
        .not_valid_after(datetime.datetime.now(datetime.UTC) + datetime.timedelta(days=3650))
        .add_extension(
            x509.SubjectAlternativeName([
                x509.UniformResourceIdentifier(
                    f"urn:instacare:device:{device_id_hex}"
                )
            ]),
            critical=False,
        )
        .add_extension(
            x509.BasicConstraints(ca=False, path_length=None),
            critical=True,
        )
        .add_extension(
            x509.KeyUsage(
                digital_signature=True,
                content_commitment=False,
                key_encipherment=False,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=False,
                crl_sign=False,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .add_extension(
            x509.AuthorityKeyIdentifier.from_issuer_public_key(
                root_key.public_key()
            ),
            critical=False,
        )
        .sign(root_key, hashes.SHA256(), default_backend())
    )

    return device_key, cert


def build_cert_chain_binary(certs):
    """
    构建协议 4.3 节格式的证书链二进制。
    格式: uint16 count + for each cert: uint32 length + DER bytes
    """
    data = bytearray()

    # count (uint16, big-endian)
    count = len(certs)
    data.append((count >> 8) & 0xFF)
    data.append(count & 0xFF)

    for cert in certs:
        der = cert.public_bytes(serialization.Encoding.DER)
        length = len(der)
        # length (uint32, big-endian)
        data.append((length >> 24) & 0xFF)
        data.append((length >> 16) & 0xFF)
        data.append((length >> 8) & 0xFF)
        data.append(length & 0xFF)
        data.extend(der)

    return bytes(data)


def format_c_array(data, device_id_hex):
    """将二进制数据格式化为 C 源文件。"""
    lines = []
    lines.append("// Auto-generated certificate chain for device")
    lines.append(f"// Device ID: {device_id_hex}")
    lines.append("// Generated by scripts/generate_certs.py")
    lines.append("// DO NOT EDIT MANUALLY")
    lines.append("")
    lines.append('#include "cert_chain.h"')
    lines.append("#include <stdint.h>")
    lines.append("#include <stddef.h>")
    lines.append("")
    lines.append("static const uint8_t s_cert_chain[] = {")

    for i in range(0, len(data), 12):
        chunk = data[i : i + 12]
        hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
        if i + 12 < len(data):
            hex_str += ","
        lines.append(f"    {hex_str}")

    lines.append("};")
    lines.append("")
    lines.append("const uint8_t *cert_chain_get_data(size_t *len)")
    lines.append("{")
    lines.append("    *len = sizeof(s_cert_chain);")
    lines.append("    return s_cert_chain;")
    lines.append("}")

    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(
        description="Generate InstaCare Peripheral test certificates"
    )
    parser.add_argument(
        "--device-id",
        required=True,
        help="32-character lowercase hex device ID",
    )
    parser.add_argument(
        "--output",
        default="main/cert_chain.c",
        help="Output C source file path (default: main/cert_chain.c)",
    )
    parser.add_argument(
        "--output-dir",
        default=".",
        help="Directory for PEM output files (default: current dir)",
    )
    args = parser.parse_args()

    device_id = args.device_id.strip().lower()

    # 验证 device_id 格式
    if len(device_id) != 32 or not all(c in "0123456789abcdef" for c in device_id):
        print(f"ERROR: device-id must be 32 hex characters, got '{args.device_id}'")
        sys.exit(1)

    print(f"Generating certificates for device: {device_id}")
    print()

    # 生成根 CA
    print("1. Generating root CA...")
    root_key, root_cert = generate_root_ca()
    print("   Root CA generated")

    # 签发设备证书
    print("2. Issuing device certificate...")
    device_key, device_cert = generate_device_cert(root_key, root_cert, device_id)
    print("   Device certificate issued")

    # 证书链：设备证书
    certs = [device_cert]

    # 构建二进制
    print("3. Building certificate chain binary...")
    chain_data = build_cert_chain_binary(certs)
    print(f"   Chain size: {len(chain_data)} bytes ({len(certs)} certificate(s))")

    # 输出 C 文件
    print(f"4. Writing {args.output}...")
    c_code = format_c_array(chain_data, device_id)
    os.makedirs(os.path.dirname(args.output) or ".", exist_ok=True)
    with open(args.output, "w") as f:
        f.write(c_code)
    print(f"   Written: {args.output}")

    # 输出 PEM 文件（供调试 / Central 测试）
    print("5. Writing PEM files...")
    out_dir = args.output_dir
    os.makedirs(out_dir, exist_ok=True)

    # 设备私钥
    with open(os.path.join(out_dir, "device_key.pem"), "wb") as f:
        f.write(
            device_key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.PKCS8,
                encryption_algorithm=serialization.NoEncryption(),
            )
        )
    print(f"   Written: {out_dir}/device_key.pem")

    # 设备证书
    with open(os.path.join(out_dir, "device_cert.pem"), "wb") as f:
        f.write(device_cert.public_bytes(serialization.Encoding.PEM))
    print(f"   Written: {out_dir}/device_cert.pem")

    # 根证书
    with open(os.path.join(out_dir, "root_cert.pem"), "wb") as f:
        f.write(root_cert.public_bytes(serialization.Encoding.PEM))
    print(f"   Written: {out_dir}/root_cert.pem")

    print()
    print("=" * 60)
    print("Done!")
    print(f"  C source:    {args.output}")
    print(f"  Root cert:   {out_dir}/root_cert.pem  (embed in Central)")
    print(f"  Device cert: {out_dir}/device_cert.pem")
    print(f"  Device key:  {out_dir}/device_key.pem  (DEBUG ONLY, keep secret)")
    print("=" * 60)


if __name__ == "__main__":
    main()