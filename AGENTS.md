# Linux port coding preferences

Use the direct style of `/home/marius/proj/allparts_new/src/` for new Linux
code and tests. Preserve the established style of upstream Rufus code, including
algorithms copied or extracted into new files. Review our adapters and newly
written logic without restyling the original implementation.

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

# Privilege prompts

Request authorization at most once per USB creation, before any writes. Keep one
authorized helper for the complete job, including cleanup and cancellation; do
not prompt separately for partitioning, formatting, mounting or bootloader work.
Keep the Qt GUI unprivileged. Do not rely on cached polkit authorization or store
passwords to meet this requirement.

# Build preferences

Let Ninja choose its default parallelism. Use `cmake --build <directory>` or
`ninja -C <directory>` without `-j`, `--parallel N`, or a
`CMAKE_BUILD_PARALLEL_LEVEL` limit unless the user explicitly requests one.
