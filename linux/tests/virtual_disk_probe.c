/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "platform/virtual_disk.h"
#include "platform/compressed_write.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
static int cancelled;
int IsRufusJobCancelled(void) { return cancelled; }
int IsIsoScanCancelled(void) { return cancelled; }
void _UpdateProgressWithInfo(int operation, int message, uint64_t processed, uint64_t total, int forced)
{ (void)operation; (void)message; (void)processed; (void)total; (void)forced; }
static unsigned Descriptors(void)
{
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) abort();
    unsigned count = 0;
    while (readdir(directory)) count++;
    closedir(directory);
    return count;
}
static int Consume(void* context, const uint8_t* data, size_t bytes, uint64_t offset)
{
    (void)data; (void)bytes; (void)offset;
    if (strcmp(context, "cancel") == 0) cancelled = 1;
    return 0;
}
int main(int argc, char** argv)
{
    if (argc != 3 && argc != 4) return 2;
    const unsigned initial = Descriptors();
    IMAGE_FILE_SOURCE source = {.fd = -1};
    COMPRESSED_IMAGE_RESULT result = {0};
    int error = OpenImageFileSource(argv[1], &source);
    const int type = GetImageCompressionType(argv[1]);
    if (error == 0 && (type == IMG_COMPRESSION_VHD || type == IMG_COMPRESSION_VHDX))
        error = OpenVirtualDiskSource(&source, type, INT64_MAX, &source.virtual_disk);
    if (error == 0 && source.virtual_disk && source.virtual_disk->parent) {
        VIRTUAL_DISK_NODE* parent = source.virtual_disk->parent;
        if (strcmp(argv[2], "mutate") == 0) {
            struct timespec stamps[2] = {parent->source.info.st_atim, parent->source.info.st_mtim}; stamps[1].tv_sec++;
            if (utimensat(AT_FDCWD, parent->path, stamps, 0) != 0) abort();
        } else if (strcmp(argv[2], "replace") == 0) {
            char moved[8192]; snprintf(moved, sizeof(moved), "%s.moved", parent->path);
            if (rename(parent->path, moved) != 0) abort();
            int fd = open(parent->path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
            if (fd < 0 || close(fd) != 0) abort();
        }
    }
    if (error == 0 && argc == 4) {
        COMPRESSED_WRITE_PLAN plan; RAW_WRITE_RESULT written = {0};
        error = PrepareCompressedWrite(&source, argv[1], INT64_MAX, 512, true, &plan);
        int target = open(argv[3], O_RDWR | O_CLOEXEC);
        if (target < 0) abort();
        if (error == 0) error = WriteCompressedImageToFile(&source, argv[1], &plan, target, &written);
        if (close(target) != 0) abort();
        if (error == 0 && written.bytes_verified != written.bytes_written) abort();
        if (error == 0 && type != IMG_COMPRESSION_FFU && written.bytes_written != plan.target_bytes) abort();
    } else if (error == 0) error = ReadCompressedImageSource(&source, argv[1], INT64_MAX, Consume, argv[2], &result);
    cancelled = 0;
    int closed = CloseVirtualDiskSource(source.virtual_disk); source.virtual_disk = NULL;
    CloseImageFileSource(&source);
    if (Descriptors() != initial) abort();
    printf("%d %d\n", error, closed);
    return 0;
}
