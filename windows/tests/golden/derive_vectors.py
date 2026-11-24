"""Derives the golden vectors for the Windows host's byte-for-byte tests.

This is an independent re-derivation from reading the Swift sources (we cannot run Swift on the PC):
  shared/WireProtocol.swift     -> header, VideoAccessUnitHeader, JSONEncoder output of each message
  shared/CryptoIdentity.swift   -> HKDF<SHA256> + HMAC<SHA256> pairing proof, ASN1X509Builder TBS bytes
  shared/PairingPayload.swift   -> kv2://pair URI, base64url, short code

Swift JSONEncoder facts used (default options, as the Swift code uses JSONEncoder() unmodified):
  * compact output, keys in declaration order (decoders never depend on order),
  * Data encodes as standard base64 with padding,
  * '/' is escaped as '\\/' (withoutEscapingSlashes is not set),
  * Bool -> true/false, integers as plain digits.

Run:  python -I windows/tests/golden/derive_vectors.py > windows/tests/golden/golden_vectors.inc
Pure standard library; the P-256 point multiply below is only used to get the public key of a
fixed test scalar (no secrets involved).
"""

import base64
import hashlib
import hmac

# ---------------------------------------------------------------- helpers

def c_bytes(name, data):
    body = ", ".join("0x%02x" % b for b in data)
    return "static const unsigned char %s[] = {%s};\nstatic const size_t %s_len = %d;\n" % (name, body, name, len(data))


def c_str(name, s):
    esc = s.replace("\\", "\\\\").replace('"', '\\"')
    return 'static const char %s[] = "%s";\n' % (name, esc)


def swift_b64(data):
    return base64.b64encode(data).decode().replace("/", "\\/")


def be32(v):
    return v.to_bytes(4, "big")


def be64(v):
    return v.to_bytes(8, "big")


out = []

# ---------------------------------------------------------------- WireProtocol.swift

# WirePacket.pack(type: .pong (0x05), flags: 0, payload: "{}")
out.append(c_bytes("kGoldenPackPong", bytes([0x4B, 0x56, 0x05, 0x00]) + be32(2) + b"{}"))

# VideoAccessUnitHeader.encode(withAvccPayload:)
au = be32(0x01020304) + be64(0x1122334455667788) + be64(0x0102030405060708) + be64(0x0A0B0C0D0E0F1011) + bytes([0x03])
au += bytes([0x00, 0x00, 0x00, 0x02, 0x65, 0x88])
out.append(c_bytes("kGoldenAuPayload", au))

hello = ('{"protocolVersion":1,"modeRaw":3,"sbsLayoutRaw":1,"allowPhoneModeSwitch":true,'
         '"width":1280,"height":424,"codecRaw":0}')
out.append(c_str("kGoldenHelloJson", hello))

sps = bytes([0x67, 0x64, 0x00, 0x1F, 0xFF, 0xEF, 0xFB, 0xF0])
pps = bytes([0x68, 0xEB, 0xE3, 0xCB, 0x22, 0xC0])
cfg = ('{"streamEpoch":2,"codecRaw":0,"width":1024,"height":848,"modeRaw":0,"sbsLayoutRaw":0,'
       '"nalLengthHeaderBytes":4,"parameterSets":["%s","%s"]}') % (swift_b64(sps), swift_b64(pps))
out.append(c_bytes("kGoldenSps", sps))
out.append(c_bytes("kGoldenPps", pps))
out.append(c_str("kGoldenStreamConfigJson", cfg))

pong = '{"clientSendNs":123456789012345,"serverRecvNs":18446744073709551615,"serverSendNs":9223372036854775813}'
out.append(c_str("kGoldenPongJson", pong))

proof32 = bytes((i * 37 + 0xF0) & 0xFF for i in range(32))
resp = ('{"accepted":true,"serviceId":"a1b2c3d4e5f6","streamPort":50123,"serverProofHMAC":"%s","message":"Paired"}'
        % swift_b64(proof32))
out.append(c_bytes("kGoldenProof32", proof32))
out.append(c_str("kGoldenPairResponseJson", resp))

reject = ('{"accepted":false,"serviceId":"","streamPort":0,"serverProofHMAC":"",'
          '"message":"Invalid pairing code or token; pairing window closed."}')
out.append(c_str("kGoldenPairRejectJson", reject))

# ---------------------------------------------------------------- CryptoIdentity.swift

def hkdf_sha256(ikm, salt, info, length):
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    okm, t, i = b"", b"", 1
    while len(okm) < length:
        t = hmac.new(prk, t + info + bytes([i]), hashlib.sha256).digest()
        okm += t
        i += 1
    return okm[:length]


def pairing_proof(secret, server_fp, client_fp, role):
    key = hkdf_sha256(secret, server_fp + client_fp, b"kv2-pair-v1", 32)
    return hmac.new(key, role.encode(), hashlib.sha256).digest()


# RFC 5869 test case 1 (sanity of the HKDF construction itself)
rfc_ikm = bytes([0x0B] * 22)
rfc_salt = bytes(range(0x00, 0x0D))
rfc_info = bytes(range(0xF0, 0xFA))
out.append(c_bytes("kRfc5869Okm", hkdf_sha256(rfc_ikm, rfc_salt, rfc_info, 42)))

server_fp = hashlib.sha256(b"server-cert").digest()
client_fp = hashlib.sha256(b"client-cert").digest()
qr_token = bytes(range(32))
short_code = b"12345678"
out.append(c_bytes("kGoldenServerFp", server_fp))
out.append(c_bytes("kGoldenClientFp", client_fp))
out.append(c_bytes("kGoldenQrToken", qr_token))
out.append(c_bytes("kGoldenProofQrClient", pairing_proof(qr_token, server_fp, client_fp, "kv2-pair-client")))
out.append(c_bytes("kGoldenProofQrServer", pairing_proof(qr_token, server_fp, client_fp, "kv2-pair-server")))
out.append(c_bytes("kGoldenProofCodeClient", pairing_proof(short_code, server_fp, client_fp, "kv2-pair-client")))
out.append(c_bytes("kGoldenProofCodeServer", pairing_proof(short_code, server_fp, client_fp, "kv2-pair-server")))

# --- P-256 public key for a fixed test scalar
P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
A = P - 3
GX = 0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296
GY = 0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5


def ec_add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    (x1, y1), (x2, y2) = p1, p2
    if x1 == x2 and (y1 + y2) % P == 0:
        return None
    if p1 == p2:
        lam = (3 * x1 * x1 + A) * pow(2 * y1, -1, P) % P
    else:
        lam = (y2 - y1) * pow(x2 - x1, -1, P) % P
    x3 = (lam * lam - x1 - x2) % P
    return (x3, (lam * (x1 - x3) - y1) % P)


def ec_mul(k, pt):
    acc = None
    while k:
        if k & 1:
            acc = ec_add(acc, pt)
        pt = ec_add(pt, pt)
        k >>= 1
    return acc


test_scalar = bytes((0x11 * (i + 1)) & 0xFF for i in range(32))
qx, qy = ec_mul(int.from_bytes(test_scalar, "big"), (GX, GY))
spki = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200") + b"\x04" + qx.to_bytes(32, "big") + qy.to_bytes(32, "big")
out.append(c_bytes("kGoldenTestScalar", test_scalar))
out.append(c_bytes("kGoldenSpki", spki))


# --- ASN1X509Builder.makeSelfSignedP256Cert TBS for that key, a fixed serial and CN "kv2-host"
def enc_len(n):
    if n < 0x80:
        return bytes([n])
    if n <= 0xFF:
        return bytes([0x81, n])
    return bytes([0x82, (n >> 8) & 0xFF, n & 0xFF])


def tlv(tag, content):
    return bytes([tag]) + enc_len(len(content)) + content


def seq(*items):
    return tlv(0x30, b"".join(items))


def set_(*items):
    return tlv(0x31, b"".join(items))


def oid(b):
    return tlv(0x06, bytes(b))


serial_raw = bytearray(range(0xA0, 0xB0))
serial_raw[0] = (serial_raw[0] & 0x7F) | 0x01
version = tlv(0xA0, tlv(0x02, bytes([0x02])))
serial = tlv(0x02, bytes(serial_raw))
sig_alg = seq(oid([0x2A, 0x86, 0x48, 0xCE, 0x3D, 0x04, 0x03, 0x02]))
name = seq(set_(seq(oid([0x55, 0x04, 0x03]), tlv(0x0C, b"kv2-host"))))
validity = seq(tlv(0x17, b"250101000000Z"), tlv(0x17, b"360101000000Z"))
ext_bc = seq(oid([0x55, 0x1D, 0x13]), tlv(0x01, b"\xff"), tlv(0x04, seq()))
ext_ku = seq(oid([0x55, 0x1D, 0x0F]), tlv(0x01, b"\xff"), tlv(0x04, tlv(0x03, bytes([7, 0x80]))))
ext_eku = seq(oid([0x55, 0x1D, 0x25]), tlv(0x04, seq(oid([0x2B, 6, 1, 5, 5, 7, 3, 1]), oid([0x2B, 6, 1, 5, 5, 7, 3, 2]))))
extensions = tlv(0xA3, seq(ext_bc, ext_ku, ext_eku))
tbs = seq(version, serial, sig_alg, name, validity, name, spki, extensions)
out.append(c_bytes("kGoldenSerialRawBeforeFixup", bytes(range(0xA0, 0xB0))))
out.append(c_bytes("kGoldenTbs", tbs))

# ---------------------------------------------------------------- PairingPayload.swift

def b64url(d):
    return base64.b64encode(d).decode().replace("+", "-").replace("/", "_").replace("=", "")


fp = bytes(range(32))
tok = bytes((0xFF - i) & 0xFF for i in range(32))
uri = "kv2://pair?s=%s&fp=%s&tok=%s&h=%s&p=%d" % ("a1b2c3d4e5f6", b64url(fp), b64url(tok), "192.168.1.20", 51234)
out.append(c_str("kGoldenQrUri", uri))
num = int.from_bytes(bytes([0x12, 0x34, 0x56, 0x78]), "big") % 100_000_000
digits = "%08d" % num
out.append(c_str("kGoldenShortCodeDigits", digits))
out.append(c_str("kGoldenShortCodeFormatted", digits[:4] + "-" + digits[4:]))

print("// Generated by windows/tests/golden/derive_vectors.py. Do not edit by hand.")
print("#pragma once")
print("#include <cstddef>")
print("".join(out), end="")
