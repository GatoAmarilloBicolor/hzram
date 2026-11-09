#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <atomic>
#include <vector>
#include <thread>
#include <chrono>
#include "zram_core.h"

extern "C" const ZramPlatformOps g_host_ops;

static std::atomic<int> g_errors{0};
static std::atomic<int> g_total_ops{0};

struct ThreadData {
    ZramDevice* dev;
    int thread_id;
    int num_ops;
    int page_start;
    int page_count;
};

void* writer_thread(void* arg) {
    ThreadData* data = (ThreadData*)arg;
    ZramDevice* dev = data->dev;
    int tid = data->thread_id;
    int ops = data->num_ops;
    int start = data->page_start;
    int count = data->page_count;

    ZramCtx* ctx = zram_ctx_get(dev);
    if (!ctx) {
        fprintf(stderr, "Thread %d: zram_ctx_get failed\n", tid);
        g_errors++;
        return NULL;
    }

    uint8_t page[4096];
    for (int op = 0; op < ops; op++) {
        int page_idx = start + (op % count);
        memset(page, tid + op, sizeof(page));
        
        int rc = zram_write_page(dev, ctx, page_idx, page);
        if (rc != ZRAM_OK) {
            fprintf(stderr, "Thread %d: write_page(%d) failed: %d\n", tid, page_idx, rc);
            g_errors++;
        }
        g_total_ops++;
    }

    zram_ctx_put(dev, ctx);
    return NULL;
}

void* reader_thread(void* arg) {
    ThreadData* data = (ThreadData*)arg;
    ZramDevice* dev = data->dev;
    int tid = data->thread_id;
    int ops = data->num_ops;
    int start = data->page_start;
    int count = data->page_count;

    uint8_t page[4096];
    uint8_t expected[4096];
    for (int op = 0; op < ops; op++) {
        int page_idx = start + (op % count);
        int expected_val = tid + op;
        memset(expected, expected_val, sizeof(expected));
        
        int rc = zram_read_page(dev, page_idx, page);
        if (rc != ZRAM_OK && rc != ZRAM_EMPTY) {
            fprintf(stderr, "Thread %d: read_page(%d) failed: %d\n", tid, page_idx, rc);
            g_errors++;
        }
        g_total_ops++;
    }

    return NULL;
}

void* mixed_thread(void* arg) {
    ThreadData* data = (ThreadData*)arg;
    ZramDevice* dev = data->dev;
    int tid = data->thread_id;
    int ops = data->num_ops;
    int start = data->page_start;
    int count = data->page_count;

    ZramCtx* ctx = zram_ctx_get(dev);
    if (!ctx) {
        fprintf(stderr, "Thread %d: zram_ctx_get failed\n", tid);
        g_errors++;
        return NULL;
    }

    uint8_t page[4096];
    for (int op = 0; op < ops; op++) {
        int page_idx = start + (op % count);
        if (op % 3 == 0) {
            memset(page, tid + op, sizeof(page));
            int rc = zram_write_page(dev, ctx, page_idx, page);
            if (rc != ZRAM_OK) {
                fprintf(stderr, "Thread %d: write_page(%d) failed: %d\n", tid, page_idx, rc);
                g_errors++;
            }
        } else {
            int rc = zram_read_page(dev, page_idx, page);
            if (rc != ZRAM_OK && rc != ZRAM_EMPTY) {
                fprintf(stderr, "Thread %d: read_page(%d) failed: %d\n", tid, page_idx, rc);
                g_errors++;
            }
        }
        g_total_ops++;
    }

    zram_ctx_put(dev, ctx);
    return NULL;
}

void* free_slots_thread(void* arg) {
    ThreadData* data = (ThreadData*)arg;
    ZramDevice* dev = data->dev;
    int tid = data->thread_id;
    int ops = data->num_ops;
    int start = data->page_start;
    int count = data->page_count;

    for (int op = 0; op < ops; op++) {
        int page_idx = start + (op % count);
        zram_free_slots(dev, page_idx, 1);
        g_total_ops++;
    }
    return NULL;
}

int run_concurrency_test(int num_threads, int ops_per_thread, int num_pages, const char* test_name) {
    ZramConfig cfg = { 16u * 1024 * 1024, 16u * 1024 * 1024, ZRAM_ENGINE_AUTO };
    ZramDevice* dev = nullptr;
    int rc = zram_create(&cfg, &g_host_ops, &dev);
    if (rc != ZRAM_OK) {
        printf("%s: zram_create failed: %d\n", test_name, rc);
        return 1;
    }

    g_errors = 0;
    g_total_ops = 0;

    pthread_t threads[32];
    ThreadData tdata[32];

    int pages_per_thread = num_pages / num_threads;

    for (int i = 0; i < num_threads; i++) {
        tdata[i] = { dev, i, ops_per_thread, i * pages_per_thread, pages_per_thread };
        int type = i % 4;
        if (type == 0) {
            pthread_create(&threads[i], NULL, writer_thread, &tdata[i]);
        } else if (type == 1) {
            pthread_create(&threads[i], NULL, reader_thread, &tdata[i]);
        } else if (type == 2) {
            pthread_create(&threads[i], NULL, mixed_thread, &tdata[i]);
        } else {
            pthread_create(&threads[i], NULL, free_slots_thread, &tdata[i]);
        }
    }

    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    zram_destroy(dev);

    printf("%s: threads=%d, ops=%d, pages=%d, errors=%d, total_ops=%d\n",
           test_name, num_threads, ops_per_thread, num_pages, g_errors.load(), g_total_ops.load());
    
    return g_errors.load() == 0 ? 0 : 1;
}

int main() {
    int failed = 0;

    // Test 1: Low contention
    failed += run_concurrency_test(4, 1000, 256, "LowContention");

    // Test 2: Medium contention
    failed += run_concurrency_test(8, 2000, 512, "MediumContention");

    // Test 3: High contention (many threads, few pages)
    failed += run_concurrency_test(16, 5000, 128, "HighContention");

    // Test 4: Many pages, fewer threads
    failed += run_concurrency_test(4, 10000, 1024, "ManyPages");

    // Test 5: Single writer, multiple readers
    {
        ZramConfig cfg = { 16u * 1024 * 1024, 16u * 1024 * 1024, ZRAM_ENGINE_AUTO };
        ZramDevice* dev = nullptr;
        int rc = zram_create(&cfg, &g_host_ops, &dev);
        if (rc != ZRAM_OK) {
            printf("SingleWriter: zram_create failed: %d\n", rc);
            failed++;
        } else {
            g_errors = 0;
            g_total_ops = 0;

            const int NUM_READERS = 8;
            const int OPS = 5000;
            pthread_t threads[NUM_READERS + 1];
            ThreadData tdata[NUM_READERS + 1];

            // One writer
            tdata[0] = { dev, 0, OPS, 0, 256 };
            pthread_create(&threads[0], NULL, writer_thread, &tdata[0]);

            // Multiple readers
            for (int i = 1; i <= NUM_READERS; i++) {
                tdata[i] = { dev, i, OPS, 0, 256 };
                pthread_create(&threads[i], NULL, reader_thread, &tdata[i]);
            }

            for (int i = 0; i <= NUM_READERS; i++) {
                pthread_join(threads[i], NULL);
            }

            zram_destroy(dev);
            printf("SingleWriter: errors=%d, total_ops=%d\n", g_errors.load(), g_total_ops.load());
            if (g_errors.load() > 0) failed++;
        }
    }

    // Test 6: Stress free_slots concurrent with read/write
    {
        ZramConfig cfg = { 16u * 1024 * 1024, 16u * 1024 * 1024, ZRAM_ENGINE_AUTO };
        ZramDevice* dev = nullptr;
        int rc = zram_create(&cfg, &g_host_ops, &dev);
        if (rc != ZRAM_OK) {
            printf("FreeStress: zram_create failed: %d\n", rc);
            failed++;
        } else {
            g_errors = 0;
            g_total_ops = 0;

            const int NUM_THREADS = 12;
            const int OPS = 3000;
            pthread_t threads[NUM_THREADS];
            ThreadData tdata[NUM_THREADS];

            for (int i = 0; i < NUM_THREADS; i++) {
                tdata[i] = { dev, i, OPS, 0, 128 };
                int type = i % 4;
                if (type == 0) pthread_create(&threads[i], NULL, writer_thread, &tdata[i]);
                else if (type == 1) pthread_create(&threads[i], NULL, reader_thread, &tdata[i]);
                else if (type == 2) pthread_create(&threads[i], NULL, mixed_thread, &tdata[i]);
                else pthread_create(&threads[i], NULL, free_slots_thread, &tdata[i]);
            }

            for (int i = 0; i < NUM_THREADS; i++) {
                pthread_join(threads[i], NULL);
            }

            zram_destroy(dev);
            printf("FreeStress: errors=%d, total_ops=%d\n", g_errors.load(), g_total_ops.load());
            if (g_errors.load() > 0) failed++;
        }
    }

    // Test 7: Context contention (many threads fighting for ctx)
    {
        ZramConfig cfg = { 8u * 1024 * 1024, 8u * 1024 * 1024, ZRAM_ENGINE_AUTO };
        ZramDevice* dev = nullptr;
        int rc = zram_create(&cfg, &g_host_ops, &dev);
        if (rc != ZRAM_OK) {
            printf("CtxContention: zram_create failed: %d\n", rc);
            failed++;
        } else {
            g_errors = 0;
            g_total_ops = 0;

            const int NUM_THREADS = 32;
            const int OPS = 1000;
            pthread_t threads[NUM_THREADS];
            ThreadData tdata[NUM_THREADS];

            for (int i = 0; i < NUM_THREADS; i++) {
                tdata[i] = { dev, i, OPS, i % 32, 32 };
                pthread_create(&threads[i], NULL, writer_thread, &tdata[i]);
            }

            for (int i = 0; i < NUM_THREADS; i++) {
                pthread_join(threads[i], NULL);
            }

            zram_destroy(dev);
            printf("CtxContention: errors=%d, total_ops=%d\n", g_errors.load(), g_total_ops.load());
            if (g_errors.load() > 0) failed++;
        }
    }

    if (failed == 0) {
        printf("\n=== ALL CONCURRENCY TESTS PASSED ===\n");
    } else {
        printf("\n=== %d TEST(S) FAILED ===\n", failed);
    }
    return failed;
}
