// Emscripten build entry point: bench-emscripten.html passes the arguments through Module.arguments.
#include <stdlib.h>

#include "bench.h"

int main(int argc, char **argv) {
    if (argc < 5) return 2;
    return bench_run(argv[1], argv[2], atoi(argv[3]), atoi(argv[4]));
}
