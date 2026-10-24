#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline verification of a real Rufus download and rejection/failure cases."""
import argparse
import base64
import errno
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile


def run_command(arguments):
    result = subprocess.run(arguments, capture_output=True, timeout=20)
    assert result.returncode == 0, (arguments, result.returncode, result.stdout, result.stderr)
    return result


def reference_public_key(header, destination):
    # Independent ASN.1 conversion; production uses OpenSSL's provider importer.
    text = header.read_text()
    initializer = text.split("\n\t0x00,", 1)[1].split("};", 1)[0]
    modulus = b"\x00" + bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})\b", initializer))
    assert len(modulus) == 257
    assert hashlib.sha256(modulus).hexdigest() == "de1a250288df6f007af1c82a2f10ffa6c23f853d595b6fc9235727bbf967df45"
    body = b"\x02\x82\x01\x01" + modulus + b"\x02\x03\x01\x00\x01"
    der = b"\x30\x82" + len(body).to_bytes(2, "big") + body
    encoded = base64.b64encode(der).decode()
    destination.write_text("-----BEGIN RSA PUBLIC KEY-----\n" +
                           "\n".join(encoded[position:position + 64] for position in range(0, len(encoded), 64)) +
                           "\n-----END RSA PUBLIC KEY-----\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--openssl", required=True)
    parser.add_argument("--key", type=Path, required=True)
    parser.add_argument("--fixtures", type=Path, required=True)
    args = parser.parse_args()
    payload = (args.fixtures / "core.img").read_bytes()
    signature = (args.fixtures / "core.img.sig").read_bytes()
    assert len(payload) == 35918 and len(signature) == 256
    assert hashlib.sha256(payload).hexdigest() == "15902895487c084c8f574666c31e7fa399d05b831fd61c1e3c3eee69d6576924"
    assert hashlib.sha256(signature).hexdigest() == "ff13d50cdd305cbd7fd1b77fea0032e5745427311850b854b9833bd9cc107c3b"
    calls = 0
    with tempfile.TemporaryDirectory(prefix="rufus-signature-") as temporary:
        root = Path(temporary)
        data_file, signature_file = root / "payload", root / "signature"
        data_file.write_bytes(payload)
        signature_file.write_bytes(signature)
        public = root / "public.pem"
        reference_public_key(args.key, public)
        run_command([args.openssl, "dgst", "-sha256", "-verify", str(public),
                     "-signature", str(signature_file), str(data_file)])
        cases = [("authentic", payload, signature, "none", 0),
                 ("empty-payload", b"", signature, "none", errno.EINVAL),
                 ("truncated-payload", payload[:-1], signature, "none", errno.EBADMSG),
                 ("extended-payload", payload + b"extra", signature, "none", errno.EBADMSG),
                 ("reversed-signature", payload, signature[::-1], "none", errno.EBADMSG),
                 ("zero-signature", payload, bytes(256), "none", errno.EBADMSG),
                 ("maximum-signature", payload, b"\xff" * 256, "none", errno.EBADMSG),
                 ("signature-too-long", payload, signature + b"extra", "none", errno.EBADMSG),
                 ("null-data", payload, signature, "null-data", errno.EINVAL),
                 ("null-signature", payload, signature, "null-signature", errno.EINVAL)]
        for length in (0, 1, 128, 255):
            cases.append((f"signature-length-{length}", payload, signature[:length], "none", errno.EBADMSG))
        for position in (0, len(payload) // 2, len(payload) - 1):
            changed = bytearray(payload)
            changed[position] ^= 1
            cases.append((f"changed-payload-{position}", bytes(changed), signature, "none", errno.EBADMSG))
        for position in (0, 127, 255):
            changed = bytearray(signature)
            changed[position] ^= 1
            cases.append((f"changed-signature-{position}", payload, bytes(changed), "none", errno.EBADMSG))
        for fault in ("modulus-allocation", "builder-allocation", "parameter-allocation", "digest-allocation"):
            cases.append((fault, payload, signature, fault, errno.ENOMEM))
        for fault in ("missing-provider", "key-import", "digest-setup", "padding-setup", "verify-error"):
            cases.append((fault, payload, signature, fault, errno.EIO))
        cases.append(("verify-rejection", payload, signature, "verify-rejection", errno.EBADMSG))
        for sequence in ("reject-then-valid", "setup-error-then-valid", "provider-error-then-valid"):
            cases.append((sequence, payload, signature, sequence, 0))
        # A correctly formed signature by an unrelated key must not be trusted.
        private = root / "wrong-key.pem"
        run_command([args.openssl, "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", str(private)])
        for name, options in (("wrong-key", []), ("wrong-key-pss", ["-sigopt", "rsa_padding_mode:pss"])):
            forged = root / (name + ".sig")
            run_command([args.openssl, "dgst", "-sha256", "-sign", str(private), *options,
                         "-out", str(forged), str(data_file)])
            cases.append((name, payload, forged.read_bytes(), "none", errno.EBADMSG))
        for name, data, sig, fault, expected in cases:
            data_file.write_bytes(data)
            signature_file.write_bytes(sig)
            result = run_command([args.probe, str(data_file), str(signature_file), fault])
            fields = {key.decode(): int(value) for key, value in re.findall(rb"([A-Z0-9_]+)=(-?\d+)", result.stdout)}
            assert fields["ERROR"] == expected and fields["UNCHANGED"] == 1, (name, fields, result.stderr)
            if expected == 0:
                assert fields["SHA256"] == 1 and fields["PADDING"] == 1, (name, fields)
            calls += 1
        # Verify the authentic pair again after restoring the mutated fixture files.
        data_file.write_bytes(payload)
        signature_file.write_bytes(signature)
        result = run_command([args.probe, str(data_file), str(signature_file), "none"])
        assert b"ERROR=0 UNCHANGED=1 SHA256=1 PADDING=1" in result.stdout
        calls += 1
    print(f"Rufus download signatures: {calls} checks passed; independent OpenSSL CLI verification passed")


if __name__ == "__main__":
    main()
