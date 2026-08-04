// Zaparoo Frontend
// Copyright (c) 2026 Wizzo Pty Ltd and the Zaparoo Project contributors.
// SPDX-License-Identifier: LicenseRef-PolyForm-Noncommercial-1.0.0

#pragma once

// The single door to the vendored display-list layer from C++.
//
// Qt's qobjectdefs.h defines `slots`, `signals` and `emit` as macros
// (empty ones, unless QT_NO_KEYWORDS is set), and the vendored glyph
// cache has a struct member called `slots` -- so including it after any
// Qt header expands that member to nothing and the header fails to
// parse. The fix cannot go upstream: those are ordinary identifiers in
// C, and the blitter has no reason to know Qt exists.
//
// Push/undef/pop rather than QT_NO_KEYWORDS project-wide, which would
// force every `slots:` in the codebase to become `Q_SLOTS:`. Include
// this header instead of reaching for the vendored ones directly, so
// the guard cannot be forgotten at a new call site.
#pragma push_macro("slots")
#pragma push_macro("signals")
#pragma push_macro("emit")
#undef slots
#undef signals
#undef emit

extern "C"
{
#include "mister-fpga-blitter/glyph_cache.h"
#include "mister-fpga-blitter/ui_offload.h"
}

#pragma pop_macro("emit")
#pragma pop_macro("signals")
#pragma pop_macro("slots")
