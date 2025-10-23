#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nng/nng.h>
#include <nng/protocol/pipeline0/push.h>

int main(int argc, char **argv)
{
    const char *addr = getenv("LISTEN_ADDR");
    if (!addr) {
        addr = (argc > 1) ? argv[1] : "tcp://repeater:7000"; // default for compose network
    }

    nng_socket s;
    int rv;
    if ((rv = nng_push0_open(&s)) != 0) {
        fprintf(stderr, "push open: %s\n", nng_strerror(rv));
        return 2;
    }
    if ((rv = nng_dial(s, addr, NULL, 0)) != 0) {
        fprintf(stderr, "dial %s: %s\n", addr, nng_strerror(rv));
        nng_close(s);
        return 3;
    }
    const char *msg = "o3ds-repeater-smoke";
    if ((rv = nng_send(s, (void*)msg, strlen(msg) + 1, 0)) != 0) {
        fprintf(stderr, "send: %s\n", nng_strerror(rv));
        nng_close(s);
        return 4;
    }
    printf("smoke push sent to %s\n", addr);
    nng_close(s);
    return 0;
}
