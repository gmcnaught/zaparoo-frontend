// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#pragma once

#include <QSize>

QT_BEGIN_NAMESPACE
class QQmlEngine;
QT_END_NAMESPACE

namespace zaparoo::fpga
{

// Try to take over rendering with the FPGA blitter.
//
// This is all-or-nothing and deliberately opt-in: the offload replaces
// the entire present path (no /dev/fb0, no per-frame memcpy, the fabric
// owns scanout), so it cannot coexist with the native video writer. It
// is enabled only when the build defines ZAPAROO_FPGA_OFFLOAD *and*
// ZAPAROO_FPGA_OFFLOAD=1 is set in the environment.
//
// Returns true if the offload is now driving the screen, in which case
// the caller must NOT load the normal QML window or start the native
// video writer. Returns false for every failure -- fabric not mapped,
// arenas too small, entry point not Item-rooted -- having logged the
// reason, and the caller carries on with the software path unchanged.
// There is no half-enabled state.
bool startOffload(QQmlEngine* engine, QSize videoSize);

// Tear down the render loop and unmap the fabric. Safe to call when
// the offload never started.
void stopOffload();

bool offloadActive();

} // namespace zaparoo::fpga
