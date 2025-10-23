#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>

#include "o3ds/pipeline.h"

static const char* kDefaultListen = "tcp://127.0.0.1:7000";

int main(int argc, char* argv[])
{
    const char* listen = std::getenv("LISTEN_ADDR");
    if (!listen) listen = (argc > 1 ? argv[1] : kDefaultListen);

    O3DS::PipelinePush push;
    if (!push.start(listen))
    {
        std::fprintf(stderr, "RepeaterSmokePush: failed to connect to %s\n", listen);
        return 2;
    }

    const char* msg = "o3ds-repeater-smoke";
    size_t len = std::strlen(msg);
    if (!push.write(msg, len))
    {
        std::fprintf(stderr, "RepeaterSmokePush: write failed\n");
        return 3;
    }

    std::printf("Sent %zu bytes to %s\n", len, listen);
    // small delay to keep process around briefly for observation
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
}
