#ifndef CTUI_H
#define CTUI_H

/* single public front door for the ctui core: apps and widgets only ever
 * #include this file. Each subsystem's real declarations live in its own
 * header under src/core/, in dependency order below -- see CLAUDE.md's
 * Architecture patterns section for what belongs in core/ vs. src/widgets/. */

#include "core/cell.h"
#include "core/deflate.h"
#include "core/gfx.h"
#include "core/screen.h"
#include "core/compositor.h"
#include "core/widget.h"
#include "core/event.h"
#include "core/timer.h"
#include "core/util.h"
#include "core/group.h"
#include "core/split.h"
#include "core/app.h"
#include "core/term.h"
#include "core/input.h"
#include "core/log.h"
#include "core/profile.h"

#endif
