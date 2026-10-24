/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		hzram developers
 */

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#ifndef ZRAM_TESTING
#define ZRAM_TESTING
#endif
#include "zram_core.h"

/* Proporcionado por platform_host.cpp */
extern "C" const ZramPlatformOps g_host_ops;
extern bool g_fail_chunk_alloc;
extern int64_t host_alloc_count(void);
extern void host_set_active_device(ZramDevice* dev);

/*
 * ============================================================================
 * UTILIDADES DE PRUEBA
 * ============================================================================
 */

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define CHECK(expr, msg)                                                \
	do {                                                                \
		if (!(expr)) {                                                  \
			fprintf(stderr, "  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
			g_tests_failed++;                                           \
			return;                                                     \
		}                                                               \
	} while (0)

#define PASS(name)                                       \
	do {                                                 \
		printf("  PASSED: %s\n", name);                  \
		g_tests_passed++;                                \
	} while (0)


static void
fill_page_pattern(uint8_t* page, uint32_t seed)
{
	uint32_t s = seed;
	for (uint32_t i = 0; i < ZRAM_PAGE_SIZE; i++) {
		s = s * 1103515245u + 12345u;
		page[i] = (uint8_t)(s >> 16);
	}
}


static void
fill_page_text(uint8_t* page, uint32_t seed)
{
	const char pattern[] = "The quick brown fox jumps over the lazy dog. ";
	size_t plen = strlen(pattern);
	for (uint32_t i = 0; i < ZRAM_PAGE_SIZE; i++)
		page[i] = (uint8_t)pattern[(i + seed) % plen];
}

/*
 * ============================================================================
 * Test 1: Crear y destruir
 * ============================================================================
 */

static void
test_create_destroy(void)
{
	printf("Test 1: Create and destroy...\n");

	ZramConfig cfg = { 4u * 1024 * 1024, ZRAM_CHUNK_BYTES };
	ZramDevice* dev = nullptr;
	int rc = zram_create(&cfg, &g_host_ops, &dev);
	CHECK(rc == ZRAM_OK, "zram_create should succeed");
	CHECK(dev != nullptr, "device should be non-NULL");

	ZramStats stats;
	zram_get_stats(dev, &stats);
	CHECK(stats.writes == 0, "initial writes should be 0");
	CHECK(stats.reads == 0, "initial reads should be 0");
	CHECK(stats.pool_bytes > 0, "pool should have been refilled");

	CHECK(zram_check_invariants(dev), "invariants after create");

	zram_destroy(dev);
	PASS("Create and destroy");
}

/*
 * ============================================================================
 * Test 2: Página de ceros (SAME con patrón 0)
 * ============================================================================
 */

static void
test_zero_page(void)
{
	printf("Test 2: Zero page...\n");

	ZramConfig cfg = { 1u * 1024 * 1024, ZRAM_CHUNK_BYTES };
	ZramDevice* dev = nullptr;
	CHECK(zram_create(&cfg, &g_host_ops, &dev) == ZRAM_OK, "create");

	uint8_t page[ZRAM_PAGE_SIZE];
	memset(page, 0, sizeof(page));

	ZramCtx* ctx = zram_ctx_get(dev);
	CHECK(ctx != nullptr, "ctx_get");

	CHECK(zram_write_page(dev, ctx, 0, page) == ZRAM_OK, "write zero page");
	CHECK(zram_test_slot_state(dev, 0) == ZRAM_SAME, "should be SAME");

	uint8_t out[ZRAM_PAGE_SIZE];
	memset(out, 0xFF, sizeof(out));
	CHECK(zram_read_page(dev, 0, out) == ZRAM_OK, "read zero page");

	for (uint32_t i = 0; i < ZRAM_PAGE_SIZE; i++)
		CHECK(out[i] == 0, "zero page byte mismatch");

	zram_ctx_put(dev, ctx);
	CHECK(zram_check_invariants(dev), "invariants");
	zram_destroy(dev);
	PASS("Zero page");
}

/*
 * ============================================================================
 * Test 3: Página con patrón de 64 bits repetido (SAME con patrón no-cero)
 * ============================================================================
 */

static void
test_same_page(void)
{
	printf("Test 3: Same-fill page...\n");

	ZramConfig cfg = { 1u * 1024 * 1024, ZRAM_CHUNK_BYTES };
	ZramDevice* dev = nullptr;
	CHECK(zram_create(&cfg, &g_host_ops, &dev) == ZRAM_OK, "create");

	uint8_t page[ZRAM_PAGE_SIZE];
	uint64_t pattern = 0xDEADBEEFCAFEBABEULL;
	for (uint32_t i = 0; i < ZRAM_PAGE_SIZE / 8; i++)
		((uint64_t*)page)[i] = pattern;

	ZramCtx* ctx = zram_ctx_get(dev);
	CHECK(zram_write_page(dev, ctx, 5, page) == ZRAM_OK, "write same page");
	CHECK(zram_test_slot_state(dev, 5) == ZRAM_SAME, "should be SAME");

	uint8_t out[ZRAM_PAGE_SIZE];
	CHECK(zram_read_page(dev, 5, out) == ZRAM_OK, "read same page");
	CHECK(memcmp(page, out, ZRAM_PAGE_SIZE) == 0, "content match");

	ZramStats stats;
	zram_get_stats(dev, &stats);
	CHECK(stats.pages_same == 1, "pages_same == 1");
	CHECK(stats.stored_bytes == 0, "SAME uses no stored bytes");

	zram_ctx_put(dev, ctx);
	CHECK(zram_check_invariants(dev), "invariants");
	zram_destroy(dev);
	PASS("Same-fill page");
}

/*
 * ============================================================================
 * Test 4: Página compresible (LZ4)
 * ============================================================================
 */

static void
test_lz4_page(void)
{
	printf("Test 4: LZ4 compressible page...\n");

	ZramConfig cfg = { 1u * 1024 * 1024, 2u * ZRAM_CHUNK_BYTES };
	ZramDevice* dev = nullptr;
	CHECK(zram_create(&cfg, &g_host_ops, &dev) == ZRAM_OK, "create");

	uint8_t page[ZRAM_PAGE_SIZE];
	fill_page_text(page, 42);

	ZramCtx* ctx = zram_ctx_get(dev);
	CHECK(zram_write_page(dev, ctx, 0, page) == ZRAM_OK, "write text page");
	CHECK(zram_test_slot_state(dev, 0) == ZRAM_LZ4, "should be LZ4");

	uint8_t out[ZRAM_PAGE_SIZE];
	CHECK(zram_read_page(dev, 0, out) == ZRAM_OK, "read text page");
	CHECK(memcmp(page, out, ZRAM_PAGE_SIZE) == 0, "roundtrip match");

	ZramStats stats;
	zram_get_stats(dev, &stats);
	CHECK(stats.pages_lz4 == 1, "pages_lz4 == 1");
	CHECK(stats.stored_bytes > 0, "stored_bytes > 0");
	CHECK(stats.stored_bytes < ZRAM_PAGE_SIZE, "compressed smaller than original");

	zram_ctx_put(dev, ctx);
	CHECK(zram_check_invariants(dev), "invariants");
	zram_destroy(dev);
	PASS("LZ4 compressible page");
}

/*
 * ============================================================================
 * Test 5: Página incompresible (RAW)
 * ============================================================================
 */

static void
test_raw_page(void)
{
	printf("Test 5: Incompressible (RAW) page...\n");

	ZramConfig cfg = { 1u * 1024 * 1024, 2u * ZRAM_CHUNK_BYTES };
	ZramDevice* dev = nullptr;
	CHECK(zram_create(&cfg, &g_host_ops, &dev) == ZRAM_OK, "create");

	uint8_t page[ZRAM_PAGE_SIZE];
	fill_page_pattern(page, 0x12345678);

	ZramCtx* ctx = zram_ctx_get(dev);
	CHECK(zram_write_page(dev, ctx, 0, page) == ZRAM_OK, "write random page");

	uint8_t state = zram_test_slot_state(dev, 0);
	CHECK(state == ZRAM_RAW || state == ZRAM_LZ4,
		"should be RAW or LZ4");

	uint8_t out[ZRAM_PAGE_SIZE];
	CHECK(zram_read_page(dev, 0, out) == ZRAM_OK, "read random page");
	CHECK(memcmp(page, out, ZRAM_PAGE_SIZE) == 0, "roundtrip match");

	zram_ctx_put(dev, ctx);
	CHECK(zram_check_invariants(dev), "invariants");
	zram_destroy(dev);
	PASS("Incompressible (RAW) page");
}

/*
 * ============================================================================
 * Test 6: Sobreescritura y transiciones de estado
 * ============================================================================
 */

static void
test_overwrite(void)
{
	printf("Test 6: Overwrite and state transitions...\n");

	ZramConfig cfg = { 1u * 1024 * 1024, 2u * ZRAM_CHUNK_BYTES };
	ZramDevice* dev = nullptr;
	CHECK(zram_create(&cfg, &g_host_ops, &dev) == ZRAM_OK, "create");

	ZramCtx* ctx = zram_ctx_get(dev);
	uint8_t page[ZRAM_PAGE_SIZE];
	uint8_t out[ZRAM_PAGE_SIZE];

	/* Escribir ceros (SAME) */
	memset(page, 0, sizeof(page));
	CHECK(zram_write_page(dev, ctx, 0, page) == ZRAM_OK, "write zero");
	CHECK(zram_test_slot_state(dev, 0) == ZRAM_SAME, "state SAME");

	/* Sobreescribir con texto (LZ4) */
	fill_page_text(page, 1);
	CHECK(zram_write_page(dev, ctx, 0, page) == ZRAM_OK, "write text");
	CHECK(zram_test_slot_state(dev, 0) == ZRAM_LZ4, "state LZ4");
	CHECK(zram_read_page(dev, 0, out) == ZRAM_OK, "read after overwrite");
	CHECK(memcmp(page, out, ZRAM_PAGE_SIZE) == 0, "text matches");

	/* Sobreescribir de nuevo con ceros (SAME) */
	memset(page, 0, sizeof(page));
	CHECK(zram_write_page(dev, ctx, 0, page) == ZRAM_OK, "write zero again");
	CHECK(zram_test_slot_state(dev, 0) == ZRAM_SAME, "state SAME again");

	ZramStats stats;
	zram_get_stats(dev, &stats);
	CHECK(stats.writes == 3, "3 writes");
	CHECK(stats.pages_same == 1, "back to 1 same");
	CHECK(stats.pages_lz4 == 0, "0 lz4 after overwrite");

	zram_ctx_put(dev, ctx);
	CHECK(zram_check_invariants(dev), "invariants");
	zram_destroy(dev);
	PASS("Overwrite and state transitions");
}

/*
 * ============================================================================
 * Test 7: Liberación de slots
 * ============================================================================
 */

static void
test_free_slots(void)
{
	printf("Test 7: Free slots...\n");

	ZramConfig cfg = { 1u * 1024 * 1024, 2u * ZRAM_CHUNK_BYTES };
	ZramDevice* dev = nullptr;
	CHECK(zram_create(&cfg, &g_host_ops, &dev) == ZRAM_OK, "create");

	ZramCtx* ctx = zram_ctx_get(dev);
	uint8_t page[ZRAM_PAGE_SIZE];

	for (uint32_t i = 0; i < 10; i++) {
		fill_page_text(page, i);
		CHECK(zram_write_page(dev, ctx, i, page) == ZRAM_OK, "write page");
	}

	ZramStats s1;
	zram_get_stats(dev, &s1);
	CHECK(s1.writes == 10, "10 writes");

	zram_free_slots(dev, 3, 5);

	ZramStats s2;
	zram_get_stats(dev, &s2);
	CHECK(s2.frees == 5, "5 frees");

	uint8_t out[ZRAM_PAGE_SIZE];
	for (uint32_t i = 3; i < 8; i++) {
		CHECK(zram_test_slot_state(dev, i) == ZRAM_EMPTY, "slot empty");
		CHECK(zram_read_page(dev, i, out) == ZRAM_OK, "read freed slot");
	}

	/* Doble liberación idempotente */
	zram_free_slots(dev, 3, 5);
	ZramStats s3;
	zram_get_stats(dev, &s3);
	CHECK(s3.frees == 5, "double-free doesn't increment");

	for (uint32_t i = 0; i < 3; i++) {
		fill_page_text(page, i);
		CHECK(zram_read_page(dev, i, out) == ZRAM_OK, "read remaining");
		CHECK(memcmp(page, out, ZRAM_PAGE_SIZE) == 0, "remaining matches");
	}

	zram_ctx_put(dev, ctx);
	CHECK(zram_check_invariants(dev), "invariants");
	zram_destroy(dev);
	PASS("Free slots");
}

/*
 * ============================================================================
 * Test 8: Manejo de falta de memoria (OOM)
 * ============================================================================
 */

static void
test_oom(void)
{
	printf("Test 8: Out-of-memory handling...\n");

	ZramConfig cfg = { 4u * 1024 * 1024, ZRAM_CHUNK_BYTES };
	ZramDevice* dev = nullptr;
	CHECK(zram_create(&cfg, &g_host_ops, &dev) == ZRAM_OK, "create");

	g_fail_chunk_alloc = true;

	ZramCtx* ctx = zram_ctx_get(dev);
	uint8_t page[ZRAM_PAGE_SIZE];

	int writesOk = 0;
	int writesFail = 0;
	for (uint32_t i = 0; i < 300; i++) {
		fill_page_pattern(page, i);
		int rc = zram_write_page(dev, ctx, i, page);
		if (rc == ZRAM_OK)
			writesOk++;
		else
			writesFail++;
	}
	CHECK(writesOk > 0, "some writes should succeed");
	CHECK(writesFail > 0, "some writes should fail (OOM)");

	for (uint32_t i = 0; i < 300; i++) {
		uint8_t out[ZRAM_PAGE_SIZE];
		uint8_t state = zram_test_slot_state(dev, i);
		if (state == ZRAM_EMPTY)
			continue;
		fill_page_pattern(page, i);
		CHECK(zram_read_page(dev, i, out) == ZRAM_OK, "read after partial OOM");
		CHECK(memcmp(page, out, ZRAM_PAGE_SIZE) == 0, "data intact after OOM");
	}

	g_fail_chunk_alloc = false;
	zram_ctx_put(dev, ctx);
	CHECK(zram_check_invariants(dev), "invariants after OOM");
	zram_destroy(dev);
	PASS("Out-of-memory handling");
}

/*
 * ============================================================================
 * Test 9: Ida y vuelta masiva con contenido mixto y desglose de estados
 * ============================================================================
 */

static void
test_bulk_roundtrip(void)
{
	printf("Test 9: Bulk roundtrip (mixed content)...\n");

	ZramConfig cfg = { 2u * 1024 * 1024, 4u * ZRAM_CHUNK_BYTES };
	ZramDevice* dev = nullptr;
	CHECK(zram_create(&cfg, &g_host_ops, &dev) == ZRAM_OK, "create");
	host_set_active_device(dev);

	const uint32_t N = 500;
	ZramCtx* ctx = zram_ctx_get(dev);
	std::vector<std::vector<uint8_t>> truth(N,
		std::vector<uint8_t>(ZRAM_PAGE_SIZE));

	for (uint32_t i = 0; i < N; i++) {
		if (i % 5 == 0) {
			/* Página de ceros */
			memset(truth[i].data(), 0, ZRAM_PAGE_SIZE);
		} else if (i % 5 == 1) {
			/* Patrón repetido */
			memset(truth[i].data(), (int)(i & 0xFF), ZRAM_PAGE_SIZE);
		} else if (i % 5 == 2) {
			/* Texto compresible */
			fill_page_text(truth[i].data(), i);
		} else {
			/* Pseudo-aleatorio incompresible */
			fill_page_pattern(truth[i].data(), i);
		}
		CHECK(zram_write_page(dev, ctx, i, truth[i].data()) == ZRAM_OK,
			"write bulk page");
	}

	uint8_t out[ZRAM_PAGE_SIZE];
	for (uint32_t i = 0; i < N; i++) {
		CHECK(zram_read_page(dev, i, out) == ZRAM_OK, "read bulk page");
		CHECK(memcmp(truth[i].data(), out, ZRAM_PAGE_SIZE) == 0,
			"bulk roundtrip mismatch");
	}

	zram_ctx_put(dev, ctx);

	ZramStats stats;
	zram_get_stats(dev, &stats);
	printf("  Detailed States Information:\n");
	printf("    SAME (identical):   %" PRIu64 " pages\n", stats.pages_same);
	printf("    LZ4  (compressed):  %" PRIu64 " pages\n", stats.pages_lz4);
	printf("    RAW  (uncompress):  %" PRIu64 " pages\n", stats.pages_raw);
	printf("    EMPTY (free slots): %" PRIu64 " slots\n", stats.pages_empty);
	printf("    Stored in RAM:      %" PRIu64 " B (Pool total: %" PRIu64 " B)\n",
		stats.stored_bytes, stats.pool_bytes);
	printf("    Class distribution (live/free):\n");
	for (uint32_t c = 0; c < ZRAM_CLASS_COUNT; c++) {
		if (stats.class_live[c] > 0 || stats.class_free[c] > 0) {
			printf("      cls %u (%4u B): %u live, %u free\n",
				c, kZramClassSize[c], stats.class_live[c], stats.class_free[c]);
		}
	}

	CHECK(zram_check_invariants(dev), "invariants after bulk");
	zram_destroy(dev);
	PASS("Bulk roundtrip");
}

/*
 * ============================================================================
 * Test 10: Validación de entradas
 * ============================================================================
 */

static void
test_validation(void)
{
	printf("Test 10: Input validation...\n");

	ZramDevice* dev = nullptr;
	ZramConfig bad1 = { 0, ZRAM_CHUNK_BYTES };
	CHECK(zram_create(&bad1, &g_host_ops, &dev) == ZRAM_E_INVAL, "zero disk_bytes");

	ZramConfig bad2 = { 4096, 0 };
	CHECK(zram_create(&bad2, &g_host_ops, &dev) == ZRAM_E_INVAL, "zero pool_limit");

	ZramConfig bad3 = { 4096, 12345 };
	CHECK(zram_create(&bad3, &g_host_ops, &dev) == ZRAM_E_INVAL, "unaligned pool_limit");

	CHECK(zram_create(nullptr, &g_host_ops, &dev) == ZRAM_E_INVAL, "null config");

	ZramConfig cfg = { 1u * 1024 * 1024, ZRAM_CHUNK_BYTES };
	CHECK(zram_create(&cfg, &g_host_ops, &dev) == ZRAM_OK, "create ok");

	ZramCtx* ctx = zram_ctx_get(dev);

	CHECK(zram_write_page(dev, ctx, 0, nullptr) == ZRAM_E_INVAL, "null page");
	CHECK(zram_write_page(dev, ctx, 999999, ctx) == ZRAM_E_INVAL, "slot out of range");
	CHECK(zram_read_page(dev, 0, nullptr) == ZRAM_E_INVAL, "read null dst");
	CHECK(zram_read_page(dev, 999999, ctx) == ZRAM_E_INVAL, "read slot out of range");

	zram_free_slots(dev, 999999, 1);
	zram_free_slots(dev, 0, 0);
	zram_free_slots(nullptr, 0, 1);

	zram_ctx_put(dev, ctx);
	zram_destroy(dev);
	PASS("Input validation");
}

/*
 * ============================================================================
 * main
 * ============================================================================
 */

int
main(void)
{
	printf("=== zram core test suite ===\n\n");

	test_create_destroy();
	test_zero_page();
	test_same_page();
	test_lz4_page();
	test_raw_page();
	test_overwrite();
	test_free_slots();
	test_oom();
	test_bulk_roundtrip();
	test_validation();

	printf("\n=== Results: %d passed, %d failed ===\n",
		g_tests_passed, g_tests_failed);

	return g_tests_failed > 0 ? 1 : 0;
}
