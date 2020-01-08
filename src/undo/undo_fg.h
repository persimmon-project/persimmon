#ifndef PSM_SRC_UNDO_UNDO_FG_H
#define PSM_SRC_UNDO_UNDO_FG_H

#include "state.h"

#include <csetjmp>

// If in recovery, recovers foreground process using memory regions sent by background.
// If it's necessary to adjust the tail, sets `tail` to the correct tail; otherwise, leaves `tail` unchanged.
// No-op if not in recovery.
int undo_recover_foreground(int *p_tail);

#endif // PSM_SRC_UNDO_UNDO_FG_H
