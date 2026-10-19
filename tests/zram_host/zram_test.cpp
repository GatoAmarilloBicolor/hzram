/*
 * zram_test.cpp - Test harness for zram core
 *
 * This is a standalone test that can be compiled and run on the host
 * system using g++. It tests the zram_core library functionality.
 */

#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <algorithm>

#include "zram_core.h"

int main(int argc, char* argv[]) {
    std::cout << "zram_test: Starting test suite..." << std::endl;

    // Test 1: Basic allocation
    std::cout << "Test 1: Basic allocation..." << std::endl;
    {
        ZramCtx ctx;
        ZramCtx* ctx_ptr = &ctx;

        if (!ZramCtx_Init(ctx_ptr, 1024 * 1024)) {
            std::cerr << "Failed: ZramCtx_Init" << std::endl;
            return 1;
        }

        ZramStats stats;
        if (!ZramStats_Init(&stats)) {
            std::cerr << "Failed: ZramStats_Init" << std::endl;
            return 1;
        }

        // Allocate a page
        void* page = malloc(ZRAM_PAGE_SIZE);
        if (!page) {
            std::cerr << "Failed: malloc" << std::endl;
            return 1;
        }

        if (!ZramCtx_Ready(ctx_ptr, page)) {
            std::cerr << "Failed: ZramCtx_Ready" << std::endl;
            return 1;
        }

        // Write data to page
        uint8_t* data = static_cast<uint8_t*>(page);
        for (uint32_t i = 0; i < ZRAM_PAGE_SIZE; i++) {
            data[i] = static_cast<uint8_t>(i);
        }

        // Read data back
        uint8_t* read_data = new uint8_t[ZRAM_PAGE_SIZE];
        if (!ZramCtx_Read(ctx_ptr, read_data)) {
            std::cerr << "Failed: ZramCtx_Read" << std::endl;
            delete[] read_data;
            return 1;
        }

        // Verify data
        for (uint32_t i = 0; i < ZRAM_PAGE_SIZE; i++) {
            if (data[i] != read_data[i]) {
                std::cerr << "Failed: Data mismatch at offset " << i << std::endl;
                delete[] read_data;
                return 1;
            }
        }

        delete[] read_data;
        std::cout << "  PASSED: Basic allocation and read/write" << std::endl;
    }

    // Test 2: LZ4 compression
    std::cout << "Test 2: LZ4 compression..." << std::endl;
    {
        ZramCtx ctx;
        ZramCtx* ctx_ptr = &ctx;

        if (!ZramCtx_Init(ctx_ptr, 1024 * 1024)) {
            std::cerr << "Failed: ZramCtx_Init" << std::endl;
            return 1;
        }

        // Create test data
        std::vector<uint8_t> test_data(1024 * 1024);
        for (uint32_t i = 0; i < 1024 * 1024; i++) {
            test_data[i] = static_cast<uint8_t>(i);
        }

        // Get pointer to data
        void* data_ptr = test_data.data();

        // Compress
        uint8_t* compressed = new uint8_t[ZRAM_MAX_COMPRESSED];
        if (!ZramCtx_Compress(ctx_ptr, data_ptr, compressed)) {
            std::cerr << "Failed: ZramCtx_Compress" << std::endl;
            delete[] compressed;
            return 1;
        }

        // Decompress
        void* decompressed = new uint8_t[ZRAM_PAGE_SIZE];
        if (!ZramCtx_Decompress(ctx_ptr, compressed, decompressed)) {
            std::cerr << "Failed: ZramCtx_Decompress" << std::endl;
            delete[] compressed;
            delete[] decompressed;
            return 1;
        }

        // Verify
        for (uint32_t i = 0; i < ZRAM_PAGE_SIZE; i++) {
            if (test_data[i] != decompressed[i]) {
                std::cerr << "Failed: Decompression mismatch at offset " << i << std::endl;
                delete[] compressed;
                delete[] decompressed;
                return 1;
            }
        }

        delete[] compressed;
        delete[] decompressed;
        std::cout << "  PASSED: LZ4 compression and decompression" << std::endl;
    }

    // Test 3: Pool management
    std::cout << "Test 3: Pool management..." << std::endl;
    {
        ZramCtx ctx;
        ZramCtx* ctx_ptr = &ctx;

        if (!ZramCtx_Init(ctx_ptr, 1024 * 1024)) {
            std::cerr << "Failed: ZramCtx_Init" << std::endl;
            return 1;
        }

        // Allocate multiple pages
        std::vector<void*> pages;
        for (int i = 0; i < 10; i++) {
            void* page = malloc(ZRAM_PAGE_SIZE);
            if (!page) {
                std::cerr << "Failed: malloc" << std::endl;
                return 1;
            }
            pages.push_back(page);

            ZramCtx_Ready(ctx_ptr, page);
        }

        // Verify all pages allocated
        for (void* page : pages) {
            ZramStats stats;
            if (!ZramStats_Ready(&stats)) {
                std::cerr << "Failed: ZramStats_Ready" << std::endl;
                return 1;
            }

            if (stats.writes != 10) {
                std::cerr << "Failed: Expected 10 writes, got " << stats.writes << std::endl;
                return 1;
            }
        }

        // Free pages
        for (void* page : pages) {
            ZramCtx_TeardownPage(ctx_ptr, page);
            free(page);
        }

        std::cout << "  PASSED: Pool management" << std::endl;
    }

    // Test 4: SIMD optimization
    std::cout << "Test 4: SIMD optimization..." << std::endl;
    {
        ZramCtx ctx;
        ZramCtx* ctx_ptr = &ctx;

        if (!ZramCtx_Init(ctx_ptr, 1024 * 1024)) {
            std::cerr << "Failed: ZramCtx_Init" << std::endl;
            return 1;
        }

        // Create test data with patterns
        std::vector<uint8_t> test_data(1024 * 1024);
        for (uint32_t i = 0; i < 1024 * 1024; i++) {
            test_data[i] = static_cast<uint8_t>((i % 256) + 128);
        }

        // Get pointer to data
        void* data_ptr = test_data.data();

        // Compress
        uint8_t* compressed = new uint8_t[ZRAM_MAX_COMPRESSED];
        if (!ZramCtx_Compress(ctx_ptr, data_ptr, compressed)) {
            std::cerr << "Failed: ZramCtx_Compress" << std::endl;
            delete[] compressed;
            return 1;
        }

        // Decompress
        void* decompressed = new uint8_t[ZRAM_PAGE_SIZE];
        if (!ZramCtx_Decompress(ctx_ptr, compressed, decompressed)) {
            std::cerr << "Failed: ZramCtx_Decompress" << std::endl;
            delete[] compressed;
            delete[] decompressed;
            return 1;
        }

        // Verify
        for (uint32_t i = 0; i < ZRAM_PAGE_SIZE; i++) {
            if (test_data[i] != decompressed[i]) {
                std::cerr << "Failed: Decompression mismatch at offset " << i << std::endl;
                delete[] compressed;
                delete[] decompressed;
                return 1;
            }
        }

        delete[] compressed;
        delete[] decompressed;
        std::cout << "  PASSED: SIMD optimization" << std::endl;
    }

    // Test 5: Compression decision
    std::cout << "Test 5: Compression decision..." << std::endl;
    {
        ZramCtx ctx;
        ZramCtx* ctx_ptr = &ctx;

        if (!ZramCtx_Init(ctx_ptr, 1024 * 1024)) {
            std::cerr << "Failed: ZramCtx_Init" << std::endl;
            return 1;
        }

        // Create test data with different patterns
        std::vector<uint8_t> test_data(4096);
        for (uint32_t i = 0; i < 4096; i++) {
            test_data[i] = static_cast<uint8_t>((i % 100) + 1);
        }

        // Get pointer to data
        void* data_ptr = test_data.data();

        // Compress
        uint8_t* compressed = new uint8_t[ZRAM_MAX_COMPRESSED];
        if (!ZramCtx_Compress(ctx_ptr, data_ptr, compressed)) {
            std::cerr << "Failed: ZramCtx_Compress" << std::endl;
            delete[] compressed;
            return 1;
        }

        // Decompress
        void* decompressed = new uint8_t[ZRAM_PAGE_SIZE];
        if (!ZramCtx_Decompress(ctx_ptr, compressed, decompressed)) {
            std::cerr << "Failed: ZramCtx_Decompress" << std::endl;
            delete[] compressed;
            delete[] decompressed;
            return 1;
        }

        // Verify
        for (uint32_t i = 0; i < 4096; i++) {
            if (test_data[i] != decompressed[i]) {
                std::cerr << "Failed: Decompression mismatch at offset " << i << std::endl;
                delete[] compressed;
                delete[] decompressed;
                return 1;
            }
        }

        delete[] compressed;
        delete[] decompressed;
        std::cout << "  PASSED: Compression decision" << std::endl;
    }

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
