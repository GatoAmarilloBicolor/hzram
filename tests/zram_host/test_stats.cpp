#include <cstdio>
#include <cstring>
#include "zram_core.h"

extern "C" const ZramPlatformOps g_host_ops;

int main() {
    ZramConfig cfg = { 4u * 1024 * 1024, 4u * 1024 * 1024, ZRAM_ENGINE_AUTO };
    ZramDevice* dev = nullptr;
    int rc = zram_create(&cfg, &g_host_ops, &dev);
    if (rc != ZRAM_OK) {
        printf("zram_create failed: %d\n", rc);
        return 1;
    }

    ZramCtx* ctx = zram_ctx_get(dev);
    if (!ctx) {
        printf("zram_ctx_get failed\n");
        zram_destroy(dev);
        return 1;
    }

    uint8_t page[4096];
    
    // Write some SAME pages (zero)
    for (int i = 0; i < 5; i++) {
        memset(page, 0, sizeof(page));
        zram_write_page(dev, ctx, i, page);
    }
    
    // Write some LZ4 compressible pages
    for (int i = 5; i < 15; i++) {
        memset(page, 'A' + (i % 26), sizeof(page));
        zram_write_page(dev, ctx, i, page);
    }
    
    // Write some RAW pages (random)
    for (int i = 15; i < 25; i++) {
        for (int j = 0; j < 4096; j++) {
            page[j] = (uint8_t)(i * 12345 + j * 6789);
        }
        zram_write_page(dev, ctx, i, page);
    }
    
    // Free some slots
    zram_free_slots(dev, 0, 2);
    
    ZramStats stats;
    zram_get_stats(dev, &stats);
    
    char buf[4096];
    zram_stats_snprintf(buf, sizeof(buf), &stats, &cfg);
    printf("%s\n", buf);
    
    zram_ctx_put(dev, ctx);
    zram_destroy(dev);
    return 0;
}
