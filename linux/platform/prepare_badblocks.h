// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "qt/prepare_image_protocol.h"
#include "badblocks_test.h"

namespace rufus
{
struct BadBlocksCallbacks
{
    void* context = nullptr;
    void (*phase)(void*, int round) = nullptr;
    void (*error)(void*, uint64_t block, unsigned type) = nullptr;
    int (*review)(void*, const BADBLOCK_RESULT&, int error) = nullptr;
    void (*complete)(void*) = nullptr;
};
// Bound only on the worker thread for one already authorized job. Native raw
// and formatting adapters use the same handler and never authorize themselves.
class BadBlocksSession
{
public:
    explicit BadBlocksSession(const BadBlocksCallbacks& callbacks);
    ~BadBlocksSession();
    bool started() const { return started_; }
    int syncError() const { return syncError_; }
    int run(const PrepareImageRequest& request, int fd);
private:
    const BadBlocksCallbacks callbacks_;
    BadBlocksSession* previous_;
    bool started_ = false;
    int syncError_ = 0;
};
int RunPrepareBadBlocks(const PrepareImageRequest& request, int fd);
}
