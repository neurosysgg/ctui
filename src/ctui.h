#ifndef CTUI_H
#define CTUI_H

/* single public front door for the ctui core: apps and widgets only ever
 * #include this file. Each subsystem's real declarations live in its own
 * header under src/core/, in dependency order below -- see CLAUDE.md's
 * Architecture patterns section for what belongs in core/ vs. src/widgets/. */

#include "core/app.h"
#include "core/cell.h"
#include "core/compositor.h"
#include "core/deflate.h"
#include "core/event.h"
#include "core/gfx.h"
#include "core/group.h"
#include "core/input.h"
#include "core/io.h"
#include "core/kitty.h"
#include "core/log.h"
#include "core/profile.h"
#include "core/screen.h"
#include "core/split.h"
#include "core/term.h"
#include "core/timer.h"
#include "core/utf8.h"
#include "core/util.h"
#include "core/widget.h"

#endif
