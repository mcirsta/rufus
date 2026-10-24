# FFU parity audit — CP56

This closes the FFU feature implementation group against the Windows Rufus
entry points in this source tree. Complete Windows Rufus parity remains active
in the [main backlog](linux-feature-parity.md). Future FFU work should address a
concrete reproducible defect or a named qualification need.

## Windows behavior and Linux coverage

| Windows entry point | Linux implementation and evidence |
| --- | --- |
| `src/format.c`, FFU branch: DISM `/Apply-Ffu /ApplyDrive:<selected drive> /ImageFile:<source>` | Ordinary image selection, authenticated source/layout, one confirmed target and one authorized helper. Full images, partial base comparisons, larger-target GPT relocation, optimized target sizing/NTFS growth/recovery updates. CP34/39/40/43/53/54/55. |
| `src/vhd.c:FfuSaveImageThread`: DISM `/Capture-Ffu` from the selected GPT drive | Read-only capture, automatic source unmount, full GPT data preservation, bounded adaptive blocks/manifests/tables, verified atomic publication. CP35/36/37/41/46/50; CP56 corrects compression to plain XPRESS. |
| Catalog policy of the desktop provider | Bounded catalog/CTL and signature verification, authenticated hashes and documented unsigned-input compatibility. CP54. |
| Completion after DISM exits | Full writes/readback, sync, private-mount cleanup and helper/launcher exit before successful READY. Combined guest evidence and existing shared helper fault matrix. |

The Linux reader additionally retains LZNT1/Huffman-XPRESS support and explicit
V2 source-store selection. These extensions do not require writing additional
target devices.

## Correction: automatic multi-drive mapping

Earlier backlog entries inferred automatic multi-target/UEFI-LUN mapping from
generic FFU store metadata. That was incorrect for the desktop provider used by
the checked Windows Rufus path. Static analysis found this call chain:

1. `InternalCmdApply` (`0x18000a620`) takes `imagefile` and `applydrive` and calls
   `CFfuManager::Apply` (`0x180005878`).
2. `FfuApplyInternal` (`0x18002337c`) creates one reader and one disk writer.
3. `CDiskWriter::CreateInstance` (`0x18001fdb4`) opens the explicitly selected
   drive through `CAbstractDisk::Initialize` (`0x18003475c`).
4. `CFfuReader::ReadStoreHeader` (`0x18003dce4`) rejects V2 `NumberOfStores != 1`.
   Its device path is logged and released; it does not resolve host devices.
5. `CDiskWriter::PreWrite` (`0x18003b7e4`) uses that same disk handle for tasks.

Therefore automatic multi-drive mapping is removed from the parity backlog.
Rufus also has no split-SFU selection, FFU optimization-creation or FFU servicing
workflow in these entry points. Generic DISM switches are not additional Rufus
features. See Microsoft's [DISM FFU command reference](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/dism-image-management-command-line-options-s14?view=windows-11).

## Capture compatibility correction

The same `ReadStoreHeader` accepts compressed-format identifiers 0 and 3.
`CRtlDecompression::Init` (`0x18003328c`) and `Decompress` (`0x180020b30`) pass
the format directly to `RtlDecompressBufferEx`. Consequently previous Linux
LZNT1 captures (identifier 2) were readable here but incompatible with this
desktop provider. CP56 saves plain XPRESS (identifier 3), with a bounded 8-KiB
history, correct paired length nibbles, literal expansion and terminal flags.
Previously saved LZNT1 inputs remain readable.

The native reader now accepts the standard terminal flag word after a complete
32-token group while preserving legacy streams without it and refusing
malformed trailers. The capture planner includes worst-case literal expansion.
The encoder follows [MS-XCA plain XPRESS processing](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-xca/b66751f2-be7b-4d20-a87c-5147c563ff2d);
independent Python/dissect decoding verifies the new outputs.

## Evidence and qualification boundary

Static profile: the owner's Windows 11 25H2 ISO, `boot.wim` index 2,
`Windows/System32/Dism/FfuProvider.dll`, version **10.0.26100.7920**, 701840 bytes,
SHA256 `87882c399d2882083fa737a1fdb1e53c8c1e0d8bdb115a24c113b8807b57e53e`.
Local analysis notes are in `.rufus-test-artifacts/cp56`; no proprietary
decompilation is included in production code. No Windows application was run.

The closeout gate uses one grouped normal/sanitizer native batch, one combined
guest per build (actual read-only capture/dirty-target restore plus one full
bootable restore), and one full Qt gate. Existing unchanged acquisition,
signature, partial-update and optimized-finalization fault evidence is reused.
Final results are recorded in [CP56](testing-and-checkpoints.md#cp56--ffu-closeout-preview-25-2026-10-20).

Existing documented bounds remain: 16-MiB decoded compressed groups and
catalogs, 64-MiB descriptor/hash tables, 4-MiB GPT entry tables, portable
optimized geometry with 128-KiB blocks and clean inspected NTFS volumes.
512/4096-byte geometry has component coverage; representative actual guest
media uses 512-byte sectors. Physical-controller coverage and execution by
other Windows/DISM versions remain unqualified. These qualification limits
are separate from the now-closed FFU feature implementation group.
