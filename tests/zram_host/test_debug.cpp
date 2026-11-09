#include <cstdio>
#include "zram_core.h"

extern "C" const ZramPlatformOps g_host_ops;

int main() {
    ZramConfig cfg = { 4u * 1024 * 1024, ZRAM_CHUNK_BYTES, ZRAM_ENGINE_AUTO };
    ZramDevice* dev = nullptr;
    int rc = zram_create(&cfg, &g_host_ops, &dev);
    printf("zram_create returned: %d\n", rc);
    if (dev) {
        ZramStats stats;
        zram_get_stats(dev, &stats);
        printf("pool_bytes: %llu\n", (unsigned long long)stats.pool_bytes);
        printf("reserver_count: %u\n", stats.reserver_count);
        printf("reserver_capacity: %u\n", stats.reserver_capacity);
        printf("chunk_count: %u\n", stats.chunk_count);
        zram_destroy(dev);
    }
    return 0;
}
