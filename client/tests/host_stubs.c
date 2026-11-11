// What nxnative's host gives the module, stood in for the tests (no game loaded)
#include <stdint.h>
const char *dm_mp_main_build(void) { return ""; }
uint8_t *dm_mp_main_base(void) { return 0; }
void gate_obs_set(int id, void (*fn)(unsigned long long *regs, int param)) { (void)id; (void)fn; }
unsigned char *dm_fps_state;   // loader.c: the frame step state page (none in the test: the module falls back to the gate and the input read)
