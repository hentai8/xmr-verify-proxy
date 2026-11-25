#!/usr/bin/env python3
"""Generate a Monero mainnet address with a valid checksum.

Only the encoding/checksum is valid - the key material is random, so nobody
(including us) can spend anything sent there. Useful as a throwaway login when
testing a pool or a proxy.

usage: python scripts/genaddr.py [--integrated] [--seed <64 hex chars>]
"""
import argparse
import hashlib
import os

try:
    from Crypto.Hash import keccak
except ImportError:  # pragma: no cover
    keccak = None

ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"


def keccak_256(data: bytes) -> bytes:
    if keccak is None:
        raise SystemExit("pycryptodome is required: python -m pip install pycryptodome")
    h = keccak.new(digest_bits=256)
    h.update(data)
    return h.digest()


def b58_block(data: bytes) -> str:
    """Monero base58: 8-byte blocks -> 11 chars, tail block -> ceil(len*8/6) chars."""
    out = []
    for i in range(0, len(data), 8):
        chunk = data[i:i + 8]
        size = (len(chunk) * 8 + 5) // 6
        value = int.from_bytes(chunk, "big")
        digits = []
        for _ in range(size):
            value, rem = divmod(value, 58)
            digits.append(ALPHABET[rem])
        if value:
            raise ValueError("block does not fit")
        out.append("".join(reversed(digits)))
    return "".join(out)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--integrated", action="store_true", help="integrated (payment id) address")
    ap.add_argument("--seed", help="deterministic seed (32 bytes hex) instead of os.urandom")
    args = ap.parse_args()

    if args.seed:
        raw = bytes.fromhex(args.seed)
        if len(raw) != 32:
            raise SystemExit("--seed must be 32 bytes of hex")
        spend = hashlib.sha256(raw + b"spend").digest()
        view = hashlib.sha256(raw + b"view").digest()
    else:
        spend = os.urandom(32)
        view = os.urandom(32)

    prefix = b"\x13" if args.integrated else b"\x12"
    payload = prefix + spend + view
    if args.integrated:
        payload += os.urandom(8)
    checksum = keccak_256(payload)[:4]
    print(b58_block(payload + checksum))


if __name__ == "__main__":
    main()
