//
// Force-included into upstream's examples/unix/c11 when they are built for the browser.
//
// z_info.c calls z_whatami_to_view_string() under Z_FEATURE_CONNECTIVITY, but upstream only
// declares it with Z_FEATURE_SCOUTING, which browsers cannot have. wasm/system.c defines it.
// eclipse-zenoh/zenoh-pico#1341
//

#ifndef ZENOH_WASM_EXAMPLES_H
#define ZENOH_WASM_EXAMPLES_H

#include "zenoh-pico.h"

#if Z_FEATURE_SCOUTING == 0 && Z_FEATURE_CONNECTIVITY == 1
z_result_t z_whatami_to_view_string(z_whatami_t whatami, z_view_string_t *str_out);
#endif

#endif /* ZENOH_WASM_EXAMPLES_H */
