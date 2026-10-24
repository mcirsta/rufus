// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/winpe_prepare.h"
#include "winpe_setup.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

extern "C" int IsRufusJobCancelled(void) { return 0; }
extern "C" int IsIsoScanCancelled(void) { return 0; }
extern "C" void uprintf(const char*, ...) { }

int main(int argc, char** argv)
{
    if (argc != 4)
        return 64;
    const int root = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0)
        return 65;
    const uint16_t mask = uint16_t(strtoul(argv[2], nullptr, 0));
    const bool usesMinint = atoi(argv[3]) != 0;
    const WINPE_SETUP_PLAN plan = GetWinPeSetupPlan(mask, usesMinint);
    const int error = rufus::PrepareWinPeRootFiles(root, mask, usesMinint, true);
    const int closeError = close(root) == 0 ? 0 : errno;
    printf("ERROR=%d CLOSE=%d MASQUERADE=%d COPY_SIF=%d\n", error, closeError,
        plan.masquerade_disk, plan.copy_sif);
    return error == 0 && closeError == 0 ? 0 : 1;
}
