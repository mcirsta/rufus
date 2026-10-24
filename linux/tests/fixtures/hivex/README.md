# Minimal registry fixture

`minimal` is the unchanged 8,192-byte `images/minimal` fixture from
[Hivex 1.3.24](https://github.com/libguestfs/hivex/tree/7996d79cf910758fcb29554e83f44c94560f170d/images),
commit `7996d79cf910758fcb29554e83f44c94560f170d`.

SHA-256: `4d97a594c11e5b56a0e0370ee181b9c7c9493ac16b7e7fc2b4e1b0199b6085e1`.

Upstream's `README.upstream` describes how this hand-edited hive was reduced to
one root node and its security descriptor. Tests copy it into a temporary
directory, add synthetic keys/values and discard the resulting hives.
It contains no installation image or user account data.

Hivex is by Richard W.M. Jones, copyright Red Hat Inc., under LGPL 2.1 only;
the upstream library license is preserved in `LICENSE`. No fixture download
occurs during configuration, build or test.
