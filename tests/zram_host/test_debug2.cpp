#include <cstdio>
#include "zram_core.h"

extern "C" const ZramPlatformOps g_host_ops;

int main() {
    ZramConfig cfg = { 4u * 1024 * 1024, ZRAM_CHUNK_BYTES, ZRAM_ENGINE_AUTO };
    ZramDevice* dev = nullptr;
    
    // Trace through zram_create manually
    if (cfg.disk_bytes == 0 || cfg.pool_limit == 0) {
        printf("FAIL: zero disk/pool\n");
        return 1;
    }
    if (cfg.pool_limit % ZRAM_CHUNK_BYTES != 0) {
        printf("FAIL: pool not aligned\n");
        return 1;
    }
    if (cfg.disk_bytes > cfg.pool_limit) {
        printf("FAIL: disk > pool: %llu > %llu\n", 
               (unsigned long long)cfg.disk_bytes, (unsigned long long)cfg.pool_limit);
        return 1;
    }
    printf("config checks passed\n");
    
    uint32_t slot_count = (uint32_t)(cfg.disk_bytes / ZRAM_PAGE_SIZE);
    printf("slot_count: %u\n", slot_count);
    if (slot_count == 0) {
        printf("FAIL: zero slots\n");
        return 1;
    }
    
    uint32_t ctx_count = g_host_ops.cpu_count();
    printf("ctx_count: %u\n", ctx_count);
    if (ctx_count == 0) ctx_count = 1;
    if (ctx_count > 256) ctx_count = 256;
    
    // Try creating
    int rc = zram_create(&cfg, &g_host_ops, &dev);
    printf("zram_create returned: %d\n", rc);
    if (dev) {
        ZramStats stats;
        zram_get_stats(dev, &stats);
        printf("pool_bytes: %llu\n", (unsigned long long)stats.pool_bytes);
        printf("reserver_count: %u\n", stats.reserver_count);
        zram_destroy(dev);
    }
    return 0;
}
