#ifndef PSM_CHKPT_H
#define PSM_CHKPT_H

#include <csetjmp>

#include <boost/filesystem.hpp>

#include <libpsm/psm.h>

using boost::filesystem::path;

struct chkpt_state {
    const path imgs_dir;
    const path service_path;
    const path log_path;
    jmp_buf restore_point;
    int seq;

    // Does NOT initialize restore_point.
    explicit chkpt_state(const psm_chkpt_config_t *config)
        : imgs_dir{config->imgs_dir}, service_path{config->service_path},
          log_path{config->log_path ? config->log_path : "/dev/null"}, restore_point{}, seq{0} {}
};

[[gnu::visibility("default")]] int chkpt_init(const chkpt_state *);
[[gnu::visibility("default")]] void chkpt_commit(chkpt_state *);

#endif // PSM_CHKPT_H
