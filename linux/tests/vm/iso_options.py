# SPDX-License-Identifier: GPL-3.0-or-later
"""One guarded device batch for ISO read options, times and the DOS override."""
import os
import subprocess


def stage_fixture(args, base, init):
    from legacy_boot import stage_fixture as stage_dos
    from prepare import write
    stage_dos(args, base, init)
    (init / 'etc/rufus-legacy-boot-fixture').unlink()
    write(init, 'etc/rufus-iso-options-fixture', 'iso-options\n')
    tree = base / 'options-source'
    entries = {'efi/boot/bootx64.efi': (args.build / 'uefi-handoff.efi').read_bytes(),
               'Mixed-Case.txt': b'ISO option payload\n', 'sub/child.txt': b'Nested timestamp payload\n'}
    for name, data in entries.items():
        path = tree / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    (tree / 'empty').mkdir()
    for path in tree.rglob('*'):
        os.utime(path, (1234567890, 1234567890))
    for name, extra in [('options.iso', []), ('options-udf.iso', ['-udf'])]:
        subprocess.run([str(args.mkisofs), '-quiet', '-R', '-J', '-V', 'OPTIONS', *extra,
                        '-o', str(init / name), str(tree)], check=True)
    (init / 'original.efi').write_bytes(entries['efi/boot/bootx64.efi'])
