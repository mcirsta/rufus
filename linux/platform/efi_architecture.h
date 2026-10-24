// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "pe_arch.h"
#include <QtEndian>
#include <cstring>
// UEFI uses ARM Thumb mixed code. Retain the original ARM spelling as well.
static inline ArchType NativeEfiArchitecture(const void* buffer, size_t size)
{
    const ArchType original = FindPeArch(buffer,size);
    if (original != ARCH_UNKNOWN || !buffer || size < 64) return original;
    const auto* bytes = static_cast<const unsigned char*>(buffer);
    if (bytes[0] != 'M' || bytes[1] != 'Z') return ARCH_UNKNOWN;
    const quint32 offset = qFromLittleEndian<quint32>(bytes+60);
    if (offset < 64 || offset > INT32_MAX || offset > size || size-offset < 24) return ARCH_UNKNOWN;
    if (memcmp(bytes+offset,"PE\0\0",4) != 0) return ARCH_UNKNOWN;
    return qFromLittleEndian<quint16>(bytes+offset+4) == 0x01c2 ? ARCH_ARM_32 : ARCH_UNKNOWN;
}
