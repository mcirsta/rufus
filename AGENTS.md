# Linux port coding preferences

Use the direct style of `/home/marius/proj/allparts_new/src/` for new Linux
code and tests. Preserve the established style of upstream Rufus code, including
algorithms copied or extracted into new files. Review our adapters and newly
written logic without restyling the original implementation.

Preserve Rufus behavior by default. A small Linux divergence is acceptable only
for a strong, concrete reason, with a coherent result for the user; implementation
convenience is not a reason. Document the original behavior, the difference and
its justification, and test it. For example, stop if a required config backup
fails rather than destroying the original after ignoring that failure.

- Give compound conditions meaningful local names, such as `fits_partition`,
  `sector_aligned` and `cluster_size_matches`. Keep each branch focused on
  related checks. Do not pack unrelated validations into a long `if` expression.
- Preserve evaluation guards when introducing helper variables: validate
  pointers before dereferencing, bounds before unsigned subtraction, and
  divisors before division. Keep system calls and error handling in clear order.
- Replace repeated magic values with constants whose names state their purpose
  and units. Derive related values and command arguments from those constants.
  Keep definitions close to their users; avoid a new constants framework.
  Test controls and fault modes need meaningful names too. Keep independent
  format expectations independent of production constants.
- Keep functions small enough to understand as useful operations. Do not split
  straightforward code into chains of three-line functions or add abstractions
  merely to reduce line counts. Small useful C helpers, including
  `MonotonicMilliseconds`, are welcome.
- Use named functions and slots, with no authored lambdas. Introduce templates
  only when they provide a substantial benefit. Existing library facilities are
  preferable to unnecessary custom infrastructure.
- Keep performance in mind: bounded buffers, batched I/O, and no unnecessary
  allocation or logging in per-sector loops.

The detailed decisions and checkpoints are in
[the port plan](docs/linux-port-plan.md) and
[the testing record](docs/testing-and-checkpoints.md).

# Feature scope

Target the complete Windows Rufus feature set, including SFD, advanced and legacy
features (decision 2026-09-25). Unimplemented is not out of scope. Select small,
testable checkpoints by dependency and usefulness; no fixed feature order is
required. Keep the user's explicit Linux differences and testing limits below.
Track remaining work in [the parity backlog](docs/linux-feature-parity.md).

# Test batching

Group related parity features that share a writer or helper into one end-to-end
qualification batch. During implementation, run focused tests for new parsers,
source identity and any new write semantics. Reuse the existing helper fault
matrix when the helper is unchanged; do not copy the whole matrix for every
format. At the batch gate, run one combined normal and sanitizer guest route:
give each format a real-device write/readback case and use one representative
full bootable restore for a shared writer. Add a format-specific full restore
only when its write semantics differ. Run the full Qt regression suite once
per batch, with focused Qt tests while developing. Preserve prewrite refusal,
claim, sync, cleanup and READY-after-exit checks where they apply.
Record one checkpoint and compact evidence package per completed batch. Avoid
separate source/helper/guest/Qt checkpoints when the same feature can be
delivered end to end in one batch. Choose the next batch by how much user
visible parity it closes and how much existing code it reuses.

# Host settings

Do not add more host detection to infer user settings, even where Windows Rufus
does so (user decision 2026-09-29). Let the user choose from Rufus's existing
lists. Existing detection may remain; do not spend more effort extending it.
This preference does not remove target/image validation or checks needed to
use available native tools safely.

# FreeDOS keyboard and code page

Use an explicit user choice on Linux, starting at US/437; do not detect or infer
it from the host desktop, XKB settings or locale (user decision 2026-09-29).
Populate choices from Rufus's shared DOS tables, and retain its startup content
and boot-time US/437 fallback. Present the chooser before authorizing/writing the
FreeDOS job; Cancel aborts preparation. Keep it in the unprivileged Qt flow and freeze
the accepted DOS values with the job request. This is separate from privileges.

# Privilege prompts

Request authorization at most once per USB creation, before any writes. Keep one
authorized helper for the complete job, including cleanup and cancellation; do
not prompt separately for partitioning, formatting, mounting or bootloader work.
Keep the Qt GUI unprivileged. Do not rely on cached polkit authorization or store
passwords to meet this requirement.

# NTFS providers

Prefer ntfsprogs-plus utilities and the new kernel `ntfs` read/write driver.
Provide an NTFS-3G fallback for systems without that kernel support. Do not
switch drivers to conceal a device, corruption or write error. Keep the FUSE
process inside the same authorized job through unmount and cancellation.

# READY means finished

Match Rufus's full green READY state and completion notification. Assume the
user will unplug the USB immediately: show successful READY only after all
writes, filesystem/device sync, private-mount cleanup and worker/helper exit
have succeeded. A sync or cleanup error must remain FAILED. Copying 100% or
receiving a helper result is not completion by itself. Do not automatically
eject or power off the USB; Windows Rufus leaves it connected. Linux removes
its own temporary private mount before completion and does not remount it
read/write just to imitate a Windows drive letter. Idle READY remains an empty
bar, as on Windows; completed READY is full. Unplugging after success must not
turn the completed job into a failure or repeat its notification.

# Build preferences

Let Ninja choose its default parallelism. Use `cmake --build <directory>` or
`ninja -C <directory>` without `-j`, `--parallel N`, or a
`CMAKE_BUILD_PARALLEL_LEVEL` limit unless the user explicitly requests one.

# Windows regional settings option

Keep Rufus's option to copy the current user's Windows regional settings visible
but disabled in the Linux Windows User Experience dialog. Explain in its tooltip
that it needs settings from a Windows host. Do not substitute Linux locale or
timezone values. The original Windows option remains supported.

All other Windows ISO customization options remain in scope for Linux parity,
including options that need boot.wim or offline registry changes. Implement them
in small checkpoints; do not drop them merely because the current Windows
backend uses Windows APIs. Only one representative customization qualification
is in scope. CP11n's real-ISO creation and independent readback with only
"Skip privacy questions" selected fulfills that scope. Its effect inside
installed Windows remains unverified and deferred. Do not schedule further
customization VM/installation tests unless the user asks; continue implementation
with focused component tests rather than a per-option Windows installation matrix.

# Test artifact cleanup

Disk space is limited. Check free space before large fixtures, and clean up
disposable disks, copied ISO trees and guest-system images when their checkpoint
finishes. Export logs, manifests, checksums and screenshots first. Keep a large
test image only when the next planned test actually needs it, and record that
reason. Preserve user-provided source ISOs and project files. Routine cleanup
of our completed test artifacts is authorized; do not let them accumulate.
