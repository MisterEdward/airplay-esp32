#!/usr/bin/env python3
"""Independent, spec-conformant AirPlay 2 transient pair-setup client.

Reproduces what the iPhone does, so the device's SRP and HKDF output can be
compared against an implementation written only from the specification.
"""
import hashlib
import os
import socket
import sys

HOST, PORT = "192.168.68.105", 7000
PRIME_BYTES = 384

N_HEX = (
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
    "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
    "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
    "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33"
    "A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864"
    "D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E2"
    "08E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF"
)
N = int(N_HEX, 16)
g = 5
USERNAME = b"Pair-Setup"
PASSWORD = b"3939"


def H(*parts):
    h = hashlib.sha512()
    for p in parts:
        h.update(p)
    return h.digest()


def int_to_bytes_min(v):
    if v == 0:
        return b"\x00"
    return v.to_bytes((v.bit_length() + 7) // 8, "big")


def pad(v, n=PRIME_BYTES):
    return v.to_bytes(n, "big")


def trim_leading_zeros(b):
    i = 0
    while i < len(b) - 1 and b[i] == 0:
        i += 1
    return b[i:]


def tlv_encode(items):
    out = bytearray()
    for t, v in items:
        if len(v) == 0:
            out += bytes([t, 0])
        for off in range(0, len(v), 255):
            chunk = v[off:off + 255]
            out += bytes([t, len(chunk)]) + chunk
    return bytes(out)


def tlv_decode(data):
    res = {}
    i = 0
    while i + 2 <= len(data):
        t, ln = data[i], data[i + 1]
        i += 2
        res.setdefault(t, bytearray())
        res[t] += data[i:i + ln]
        i += ln
    return {k: bytes(v) for k, v in res.items()}


def rtsp_post(sock, path, body, cseq):
    req = (
        f"POST {path} RTSP/1.0\r\n"
        f"CSeq: {cseq}\r\n"
        f"User-Agent: AirPlay/770.8.1\r\n"
        f"Content-Type: application/octet-stream\r\n"
        f"Content-Length: {len(body)}\r\n\r\n"
    ).encode() + body
    sock.sendall(req)

    buf = b""
    while b"\r\n\r\n" not in buf:
        c = sock.recv(4096)
        if not c:
            raise RuntimeError("connection closed reading headers")
        buf += c
    head, rest = buf.split(b"\r\n\r\n", 1)
    clen = 0
    for line in head.decode("utf-8", "replace").split("\r\n"):
        if line.lower().startswith("content-length:"):
            clen = int(line.split(":", 1)[1].strip())
    while len(rest) < clen:
        c = sock.recv(4096)
        if not c:
            raise RuntimeError("connection closed reading body")
        rest += c
    return head.decode("utf-8", "replace"), rest[:clen]


def hkdf_sha512(salt, ikm, info, length):
    prk = hashlib.pbkdf2_hmac  # placeholder to keep linters quiet
    import hmac as _hmac
    prk = _hmac.new(salt, ikm, hashlib.sha512).digest()
    okm, t = b"", b""
    counter = 1
    while len(okm) < length:
        t = _hmac.new(prk, t + info + bytes([counter]), hashlib.sha512).digest()
        okm += t
        counter += 1
    return okm[:length]


def main():
    s = socket.create_connection((HOST, PORT), timeout=15)
    s.settimeout(20)

    # ---- M1: request transient pair-setup -------------------------------
    m1 = tlv_encode([(0x06, b"\x01"), (0x13, b"\x10")])
    head, body = rtsp_post(s, "/pair-setup", m1, 1)
    print("M2 status:", head.split("\r\n")[0])
    t = tlv_decode(body)
    if 0x07 in t:
        print("!! device returned error TLV:", t[0x07].hex())
        return 1
    salt = t[0x02]
    B_bytes = t[0x03]
    B = int.from_bytes(B_bytes, "big")
    print(f"salt={salt.hex()} B_len={len(B_bytes)}")

    # ---- client-side SRP -------------------------------------------------
    k = int.from_bytes(H(pad(N), pad(g)), "big")
    x = int.from_bytes(H(salt, H(USERNAME + b":" + PASSWORD)), "big")

    a = int.from_bytes(os.urandom(32), "big")
    A = pow(g, a, N)
    A_bytes = pad(A)

    u = int.from_bytes(H(A_bytes, pad(B)), "big")

    # S = (B - k*g^x)^(a + u*x) mod N
    S = pow((B - k * pow(g, x, N)) % N, a + u * x, N)
    S_bytes = int_to_bytes_min(S)
    K = H(S_bytes)

    h_N = H(pad(N))
    h_g = H(int_to_bytes_min(g))
    h_Ng_xor = bytes(p ^ q for p, q in zip(h_N, h_g))
    h_I = H(USERNAME)

    M1proof = H(
        h_Ng_xor,
        h_I,
        trim_leading_zeros(salt),
        int_to_bytes_min(A),
        int_to_bytes_min(B),
        K,
    )

    print(f"CLIENT K   = {K.hex()}")
    print(f"CLIENT K[0:8] = {K[:8].hex()}")

    # ---- M3 --------------------------------------------------------------
    m3 = tlv_encode([(0x06, b"\x03"), (0x03, A_bytes), (0x04, M1proof)])
    head, body = rtsp_post(s, "/pair-setup", m3, 2)
    print("M4 status:", head.split("\r\n")[0])
    t = tlv_decode(body)
    if 0x07 in t:
        print("!! device rejected M3, error TLV:", t[0x07].hex())
        print("   -> our K/M1 does not match the device's")
        return 1
    print("M3 ACCEPTED by device -> device agrees on K")

    # ---- expected control keys ------------------------------------------
    keys = {}
    for name, ikm, label in (
        ("read64", K, b"Control-Write-Encryption-Key"),
        ("write64", K, b"Control-Read-Encryption-Key"),
        ("read32", K[:32], b"Control-Write-Encryption-Key"),
        ("write32", K[:32], b"Control-Read-Encryption-Key"),
    ):
        keys[name] = hkdf_sha512(b"Control-Salt", ikm, label, 32)
        print(f"PY {name:8s} = {keys[name].hex()}")

    # ---- behavioural test: send one encrypted frame ----------------------
    # Encrypt with the key the accessory should be using to DECRYPT. If the
    # device answers, its derivation matches the specification; if it drops
    # the connection, its derivation differs from an independent one.
    from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305

    which = sys.argv[1] if len(sys.argv) > 1 else "read64"
    plaintext = b"GET /info RTSP/1.0\r\nCSeq: 3\r\nUser-Agent: AirPlay/770.8.1\r\n\r\n"
    nonce = b"\x00" * 12
    aad = len(plaintext).to_bytes(2, "little")
    ct = ChaCha20Poly1305(keys[which]).encrypt(nonce, plaintext, aad)
    print(f"\n>> sending {len(plaintext)}-byte frame encrypted with '{which}'")
    s.sendall(aad + ct)

    s.settimeout(8)
    try:
        resp = s.recv(4096)
        if resp:
            print(f"<< DEVICE REPLIED with {len(resp)} bytes: {resp[:16].hex()}")
            print("   => device decrypted our frame: its derivation MATCHES spec")
        else:
            print("<< connection closed by device (decrypt rejected)")
    except socket.timeout:
        print("<< no reply within 8s (decrypt rejected)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
