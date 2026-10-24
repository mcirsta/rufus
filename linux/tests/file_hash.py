# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded fixture hashing on Python versions before hashlib.file_digest."""
import hashlib


def file_sha256(stream):
    if hasattr(hashlib, 'file_digest'):
        return hashlib.file_digest(stream, 'sha256')
    digest = hashlib.sha256()
    while True:
        block = stream.read(256 * 1024)
        if not block:
            return digest
        digest.update(block)
