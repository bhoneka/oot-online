// OoT Online: the two facts the game-side code needs from nxnative's loader (dm.h), so the game files need no host headers
#include "dm.h"
const char *dm_mp_main_build(void) { return dm_main_build(); }
uint8_t *dm_mp_main_base(void) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) return g_mod[i].base; return NULL; }
