/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "windows_registry.h"
#include "rufus_reporting.h"
#include "unattend.h"

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <hivex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { DESCRIPTOR_PATH_CAPACITY = 64 };

static hive_h* OpenSourceHive(const char* path)
{
	struct stat info;
	char descriptor_path[DESCRIPTOR_PATH_CAPACITY];
	hive_h* hive = NULL;
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	int error = 0;
	if (fstat(fd, &info) != 0)
		error = errno;
	else if (!S_ISREG(info.st_mode))
		error = EINVAL;
	if (error == 0) {
		snprintf(descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d", fd);
		/* WRITE loads an editable memory copy; commit gets a separate output. */
		hive = hivex_open(descriptor_path, HIVEX_OPEN_WRITE);
		if (hive == NULL)
			error = errno;
	}
	close(fd);
	errno = error;
	return hive;
}

static int SetSetupValues(hive_h* hive)
{
	hive_node_h root = hivex_root(hive);
	if (root == 0)
		return errno;
	errno = 0;
	hive_node_h setup = hivex_node_get_child(hive, root, "Setup");
	if (setup == 0)
		return errno != 0 ? errno : ENOENT;
	errno = 0;
	hive_node_h config = hivex_node_get_child(hive, setup, "LabConfig");
	if (config == 0 && errno == 0)
		config = hivex_node_add_child(hive, setup, "LabConfig");
	if (config == 0)
		return errno;
	uint32_t enabled = htole32(1);
	for (int i = 0; i < UNATTEND_BYPASS_COUNT; ++i) {
		if (IsRufusJobCancelled())
			return ECANCELED;
		/* Hivex borrows these bytes and strings without modifying them. */
		hive_set_value value = { (char*)bypass_name[i], hive_t_REG_DWORD,
			sizeof(enabled), (char*)&enabled };
		if (hivex_node_set_value(hive, config, &value, 0) != 0)
			return errno;
	}
	return 0;
}

static hive_node_h HiveChild(hive_h* hive, hive_node_h parent, const char* name, bool create)
{
	errno = 0;
	hive_node_h child = hivex_node_get_child(hive, parent, name);
	if (child == 0 && errno == 0 && create)
		child = hivex_node_add_child(hive, parent, name);
	if (child == 0 && errno == 0) errno = ENOENT;
	return child;
}

static hive_node_h WindowsDefaultControlSet(hive_h* hive)
{
	hive_node_h node = hivex_root(hive);
	if (node == 0) { if (errno == 0) errno = EBADMSG; return 0; }
	const hive_node_h root = node;
	node = HiveChild(hive, node, "Select", false);
	if (node == 0) return 0;
	errno = 0;
	const hive_value_h selected = hivex_node_get_value(hive, node, "Default");
	if (selected == 0) { if (errno == 0) errno = ENOENT; return 0; }
	hive_type type;
	size_t length;
	char* bytes = hivex_value_value(hive, selected, &type, &length);
	if (bytes == NULL) return 0;
	uint32_t number = 0;
	if (type == hive_t_REG_DWORD && length == sizeof(number))
		memcpy(&number, bytes, sizeof(number));
	free(bytes);
	number = le32toh(number);
	if (number == 0 || number > 999) { errno = EBADMSG; return 0; }
	char control_set[sizeof("ControlSet999")];
	snprintf(control_set, sizeof(control_set), "ControlSet%03u", number);
	return HiveChild(hive, root, control_set, false);
}

static int SetWindowsToGoValues(hive_h* hive, int flags)
{
	const hive_node_h control_set = WindowsDefaultControlSet(hive);
	if (control_set == 0) return errno;
	if (flags & UNATTEND_OFFLINE_INTERNAL_DRIVES) {
		hive_node_h node = control_set;
		const char* const path[] = {"Services", "partmgr", "Parameters"};
		for (size_t i = 0; i < sizeof(path) / sizeof(path[0]); i++) {
			if (IsRufusJobCancelled()) return ECANCELED;
			/* Require the existing partition manager; its Parameters may be new. */
			node = HiveChild(hive, node, path[i], i == 2);
			if (node == 0) return errno;
		}
		uint32_t policy = htole32(4);
		hive_set_value value = {(char*)"SanPolicy", hive_t_REG_DWORD, sizeof(policy), (char*)&policy};
		if (hivex_node_set_value(hive, node, &value, 0) != 0) return errno;
	}
	if (flags & UNATTEND_FORCE_S_MODE) {
		hive_node_h node = control_set;
		const char* const path[] = {"Control", "CI", "Policy"};
		for (size_t i = 0; i < sizeof(path) / sizeof(path[0]); i++) {
			if (IsRufusJobCancelled()) return ECANCELED;
			node = HiveChild(hive, node, path[i], i != 0);
			if (node == 0) return errno;
		}
		/* Microsoft-Windows-CodeIntegrity's offlineServicing registry binding. */
		uint32_t policy = htole32(1);
		hive_set_value value = {(char*)"SkuPolicyRequired", hive_t_REG_DWORD, sizeof(policy), (char*)&policy};
		if (hivex_node_set_value(hive, node, &value, 0) != 0) return errno;
	}
	return 0;
}

static int PrepareHive(const char* source, const char* destination, int to_go_flags)
{
	const bool valid_source = source != NULL && *source != 0;
	const bool valid_destination = destination != NULL && *destination != 0;
	if (!valid_source || !valid_destination)
		return EINVAL;
	if (IsRufusJobCancelled())
		return ECANCELED;
	hive_h* hive = OpenSourceHive(source);
	if (hive == NULL)
		return errno;
	int fd = -1;
	char descriptor_path[DESCRIPTOR_PATH_CAPACITY];
	int error = to_go_flags != 0 ? SetWindowsToGoValues(hive, to_go_flags) : SetSetupValues(hive);
	if (error != 0)
		goto out;
	if (IsRufusJobCancelled()) {
		error = ECANCELED;
		goto out;
	}
	fd = open(destination, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (fd < 0) {
		error = errno;
		goto out;
	}
	snprintf(descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d", fd);
	if (hivex_commit(hive, descriptor_path, 0) != 0)
		error = errno;
	if (error == 0 && fsync(fd) != 0)
		error = errno;
out:
	if (hivex_close(hive) != 0 && error == 0)
		error = errno;
	if (fd >= 0 && close(fd) != 0 && error == 0)
		error = errno;
	if (error == 0 && IsRufusJobCancelled())
		error = ECANCELED;
	if (error != 0 && fd >= 0 && unlink(destination) != 0)
		uprintf("Could not remove the incomplete SYSTEM hive: %s", strerror(errno));
	return error;
}

int PrepareWindowsSetupHive(const char* source, const char* destination)
{
	return PrepareHive(source, destination, 0);
}

int PrepareWindowsToGoHive(const char* source, const char* destination, int flags)
{
	if (flags == 0 || (flags & ~UNATTEND_OFFLINE_SERVICING_MASK)) return EINVAL;
	return PrepareHive(source, destination, flags);
}
