/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Disposable-file fixture only. No device, mount or authorization operations. */
#include "platform/config_patch.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int IsIsoScanCancelled(void) { return 0; }

int main(int argc, char** argv)
{
	bool modified = false;
	int parent, error;
	if (argc != 6)
		return 64;
	parent = open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (parent < 0)
		return 65;
	error = ReplaceConfigTokenAt(parent, argv[2], argv[3], argv[4], argv[5], &modified);
	if (close(parent) != 0 && error == 0)
		error = errno;
	printf("ERROR=%d MODIFIED=%d\n", error, modified);
	return 0;
}
