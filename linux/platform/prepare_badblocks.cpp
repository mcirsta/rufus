// SPDX-License-Identifier: GPL-3.0-or-later
#include "prepare_badblocks.h"
#include "rufus_reporting.h"
#include <cerrno>
#include <climits>

namespace rufus
{
namespace { thread_local BadBlocksSession* session = nullptr; }
BadBlocksSession::BadBlocksSession(const BadBlocksCallbacks& callbacks) : callbacks_(callbacks), previous_(session) { session = this; }
BadBlocksSession::~BadBlocksSession() { session = previous_; }
int BadBlocksSession::run(const PrepareImageRequest& request, int fd)
{
    if (!callbacks_.phase || !callbacks_.error || !callbacks_.review) return EINVAL;
    int error = 0;
    for (int round = 1; error == 0; round++)
    {
        callbacks_.phase(callbacks_.context, round);
        BADBLOCK_RESULT result;
        error = TestBadBlocksDevice(fd, request.device, request.size, request.badBlocksPattern,
            request.detectFakes, callbacks_.error, callbacks_.context, &result);
        started_ = started_ || result.bytes_written != 0;
        if (result.sync_error != 0) syncError_ = result.sync_error;
        const int decision = callbacks_.review(callbacks_.context, result, error);
        if (error != 0) break;
        if (result.count == 0 || decision == BadBlocksIgnore) break;
        if (decision != BadBlocksRetry || IsRufusJobCancelled()) { error = ECANCELED; break; }
        if (round == INT_MAX) { error = EOVERFLOW; break; }
    }
    if (started_)
    {
        int sync = 0;
        const int cleared = ClearBadBlocksHeaders(fd, request.device, request.size, &sync);
        if (sync != 0) syncError_ = sync;
        if (cleared != 0 && (error == 0 || error == ECANCELED)) error = cleared;
    }
    if (error == 0 && callbacks_.complete) callbacks_.complete(callbacks_.context);
    return error;
}
int RunPrepareBadBlocks(const PrepareImageRequest& request, int fd)
{
    if (request.badBlocksPattern < 0) return 0;
    if (!request.isDevice || request.badBlocksPattern >= BADLOCKS_PATTERN_TYPES || !session) return EINVAL;
    return session->run(request, fd);
}
}
