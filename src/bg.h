#ifndef PSM_UNDO_BG_H
#define PSM_BG_H

#include <libpsm/psm.h>

[[gnu::noreturn]] void bg_run(psm_t *psm, bool use_sga);

#endif // PSM_UNDO_BG_H
