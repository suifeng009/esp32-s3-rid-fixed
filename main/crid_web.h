#ifndef CRID_WEB_H
#define CRID_WEB_H

#include <stdbool.h>
#include "crid_config.h"

#ifdef __cplusplus
extern "C" {
#endif

void crid_web_init(void);
void crid_web_start(void);
cn_crid_config_t *crid_web_get_sim_config(void);
bool crid_web_is_sim_running(void);
void crid_web_set_sim_running(bool running);

#ifdef __cplusplus
}
#endif
#endif
