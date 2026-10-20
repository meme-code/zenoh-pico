//
// Sends a batched burst and closes the session right away, single-threaded, like
// bench/bench.c's pubbatch mode: everything queued before the close must still reach the
// router. Run by tests/programs-test.html, which counts the samples.
//
//   burst-close-st <locator> <count>
//

#include <stdio.h>
#include <stdlib.h>
#include <zenoh-pico.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: burst-close-st <locator> <count>\n");
        return 2;
    }
    int n = atoi(argv[2]);
    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, argv[1]);
    z_owned_session_t s;
    if (z_open(&s, z_move(config), NULL) < 0) {
        printf("Unable to open session!\n");
        return 1;
    }
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str(&ke, "prog/burst");
    z_publisher_options_t opts;
    z_publisher_options_default(&opts);
    opts.congestion_control = Z_CONGESTION_CONTROL_BLOCK;
    z_owned_publisher_t pub;
    if (z_declare_publisher(z_loan(s), &pub, z_loan(ke), &opts) < 0) {
        printf("Unable to declare publisher!\n");
        return 1;
    }
    // Let the router report the subscriber: the publisher's write filter starts closed.
    for (int i = 0; i < 50; i++) {
        zp_spin_once(z_loan(s));
        z_sleep_ms(10);
    }
    zp_batch_start(z_loan(s));
    for (int i = 0; i < n; i++) {
        z_owned_bytes_t payload;
        z_bytes_copy_from_str(&payload, "a message in a burst that is closed at once");
        z_publisher_put(z_loan(pub), z_move(payload), NULL);
    }
    zp_batch_stop(z_loan(s));
    z_drop(z_move(pub));
    z_drop(z_move(s));
    printf("sent %d\n", n);
    return 0;
}
