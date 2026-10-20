#include "core/vector_engine.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <immintrin.h>
#include <pthread.h>
#include <errno.h>

#define CACHE_LINE_SIZE 64
#define ALIGN_UP(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

typedef struct VectorBatchInternal {
    uint32_t capacity;
    uint32_t size;
    uint64_t* validity_mask;
    uint16_t* selection_vector;
    void** data_columns;
    uint32_t num_columns;
    bool own_memory;
} VectorBatchInternal;

typedef struct ColumnChunkInternal {
    uint32_t row_count;
    uint32_t capacity;
    uint8_t compression_type;
    uint32_t crc32;
    uint8_t* data;
    size_t data_size;
    uint64_t* null_bitmap;
    pthread_rwlock_t latch;
    bool sealed;
} ColumnChunkInternal;

static uint32_t calculate_crc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (uint32_t j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & ~((crc & 1) - 1));
        }
    }
    return ~crc;
}

VectorBatch* vector_batch_create(uint32_t capacity, uint32_t num_columns) {
    if (capacity == 0 || num_columns == 0) {
        return NULL;
    }

    size_t total_size = ALIGN_UP(sizeof(VectorBatchInternal), CACHE_LINE_SIZE) +
                        ALIGN_UP(num_columns * sizeof(void*), CACHE_LINE_SIZE) +
                        ALIGN_UP(capacity * sizeof(uint16_t), CACHE_LINE_SIZE) +
                        ALIGN_UP(((capacity + 63) / 64) * sizeof(uint64_t), CACHE_LINE_SIZE);

    uint8_t* raw_mem = (uint8_t*)aligned_alloc(CACHE_LINE_SIZE, ALIGN_UP(total_size, CACHE_LINE_SIZE));
    if (!raw_mem) {
        return NULL;
    }
    memset(raw_mem, 0, total_size);

    VectorBatchInternal* batch = (VectorBatchInternal*)raw_mem;
    batch->capacity = capacity;
    batch->size = 0;
    batch->num_columns = num_columns;
    batch->own_memory = true;

    size_t offset = ALIGN_UP(sizeof(VectorBatchInternal), CACHE_LINE_SIZE);
    batch->data_columns = (void**)(raw_mem + offset);

    offset += ALIGN_UP(num_columns * sizeof(void*), CACHE_LINE_SIZE);
    batch->selection_vector = (uint16_t*)(raw_mem + offset);

    offset += ALIGN_UP(capacity * sizeof(uint16_t), CACHE_LINE_SIZE);
    batch->validity_mask = (uint64_t*)(raw_mem + offset);

    for (uint32_t i = 0; i < num_columns; i++) {
        size_t col_bytes = ALIGN_UP(capacity * sizeof(float), CACHE_LINE_SIZE);
        void* col_data = aligned_alloc(CACHE_LINE_SIZE, col_bytes);
        if (!col_data) {
            for (uint32_t j = 0; j < i; j++) {
                free(batch->data_columns[j]);
            }
            free(raw_mem);
            return NULL;
        }
        memset(col_data, 0, col_bytes);
        batch->data_columns[i] = col_data;
    }

    return (VectorBatch*)batch;
}

void vector_batch_destroy(VectorBatch* batch_ptr) {
    if (!batch_ptr) {
        return;
    }
    VectorBatchInternal* batch = (VectorBatchInternal*)batch_ptr;
    if (batch->own_memory) {
        for (uint32_t i = 0; i < batch->num_columns; i++) {
            free(batch->data_columns[i]);
        }
        free(batch);
    }
}

void vector_batch_reset(VectorBatch* batch_ptr) {
    if (!batch_ptr) {
        return;
    }
    VectorBatchInternal* batch = (VectorBatchInternal*)batch_ptr;
    batch->size = 0;
    size_t validity_bytes = ((batch->capacity + 63) / 64) * sizeof(uint64_t);
    memset(batch->validity_mask, 0, validity_bytes);
    memset(batch->selection_vector, 0, batch->capacity * sizeof(uint16_t));
}

ColumnChunk* column_chunk_create(uint32_t capacity, uint8_t compression_type) {
    if (capacity == 0) {
        return NULL;
    }

    size_t struct_size = ALIGN_UP(sizeof(ColumnChunkInternal), CACHE_LINE_SIZE);
    size_t null_bytes = ALIGN_UP(((capacity + 63) / 64) * sizeof(uint64_t), CACHE_LINE_SIZE);
    size_t data_bytes = ALIGN_UP(capacity * sizeof(float), CACHE_LINE_SIZE);

    size_t total_size = struct_size + null_bytes + data_bytes;
    uint8_t* raw_mem = (uint8_t*)aligned_alloc(CACHE_LINE_SIZE, ALIGN_UP(total_size, CACHE_LINE_SIZE));
    if (!raw_mem) {
        return NULL;
    }
    memset(raw_mem, 0, total_size);

    ColumnChunkInternal* chunk = (ColumnChunkInternal*)raw_mem;
    chunk->row_count = 0;
    chunk->capacity = capacity;
    chunk->compression_type = compression_type;
    chunk->crc32 = 0;
    chunk->sealed = false;

    size_t offset = struct_size;
    chunk->null_bitmap = (uint64_t*)(raw_mem + offset);

    offset += null_bytes;
    chunk->data = raw_mem + offset;
    chunk->data_size = data_bytes;

    if (pthread_rwlock_init(&chunk->latch, NULL) != 0) {
        free(raw_mem);
        return NULL;
    }

    return (ColumnChunk*)chunk;
}

void column_chunk_destroy(ColumnChunk* chunk_ptr) {
    if (!chunk_ptr) {
        return;
    }
    ColumnChunkInternal* chunk = (ColumnChunkInternal*)chunk_ptr;
    pthread_rwlock_destroy(&chunk->latch);
    free(chunk);
}

int32_t column_chunk_append(ColumnChunk* chunk_ptr, const void* data, uint32_t count, const uint64_t* null_bitmap) {
    if (!chunk_ptr || !data || count == 0) {
        return -EINVAL;
    }

    ColumnChunkInternal* chunk = (ColumnChunkInternal*)chunk_ptr;

    if (pthread_rwlock_wrlock(&chunk->latch) != 0) {
        return -EBUSY;
    }

    if (chunk->sealed || (chunk->row_count + count > chunk->capacity)) {
        pthread_rwlock_unlock(&chunk->latch);
        return -ENOMEM;
    }

    uint32_t start_row = chunk->row_count;
    memcpy(chunk->data + (start_row * sizeof(float)), data, count * sizeof(float));

    if (null_bitmap) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t target_row = start_row + i;
            uint32_t word_idx = target_row / 64;
            uint32_t bit_idx = target_row % 64;
            uint32_t src_word = i / 64;
            uint32_t src_bit = i % 64;

            bool is_valid = (null_bitmap[src_word] >> src_bit) & 1ULL;
            if (is_valid) {
                chunk->null_bitmap[word_idx] |= (1ULL << bit_idx);
            } else {
                chunk->null_bitmap[word_idx] &= ~(1ULL << bit_idx);
            }
        }
    } else {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t target_row = start_row + i;
            uint32_t word_idx = target_row / 64;
            uint32_t bit_idx = target_row % 64;
            chunk->null_bitmap[word_idx] |= (1ULL << bit_idx);
        }
    }

    chunk->row_count += count;
    chunk->crc32 = calculate_crc32(chunk->data, chunk->row_count * sizeof(float));

    pthread_rwlock_unlock(&chunk->latch);
    return 0;
}

int32_t column_chunk_seal(ColumnChunk* chunk_ptr) {
    if (!chunk_ptr) {
        return -EINVAL;
    }

    ColumnChunkInternal* chunk = (ColumnChunkInternal*)chunk_ptr;

    if (pthread_rwlock_wrlock(&chunk->latch) != 0) {
        return -EBUSY;
    }

    chunk->sealed = true;
    chunk->crc32 = calculate_crc32(chunk->data, chunk->row_count * sizeof(float));

    pthread_rwlock_unlock(&chunk->latch);
    return 0;
}

int32_t vector_engine_execute_filter_simd(const VectorBatch* batch_ptr, uint32_t col_idx, float threshold, VectorBatch* out_batch_ptr) {
    if (!batch_ptr || !out_batch_ptr) {
        return -EINVAL;
    }

    const VectorBatchInternal* batch = (const VectorBatchInternal*)batch_ptr;
    VectorBatchInternal* out_batch = (VectorBatchInternal*)out_batch_ptr;

    if (col_idx >= batch->num_columns) {
        return -EINVAL;
    }

    const float* data = (const float*)batch->data_columns[col_idx];
    uint32_t size = batch->size;
    uint32_t match_count = 0;

    __m512 thresh_vec = _mm512_set1_ps(threshold);
    uint32_t i = 0;

    for (; i + 16 <= size; i += 16) {
        __m512 vals = _mm512_loadu_ps(data + i);
        __mmask16 mask = _mm512_cmp_ps_mask(vals, thresh_vec, _MM_CMPINT_GT);

        while (mask) {
            uint32_t bit_pos = __builtin_ctz(mask);
            uint16_t row_idx = (uint16_t)(i + bit_pos);
            out_batch->selection_vector[match_count++] = row_idx;
            mask &= mask - 1;
        }
    }

    for (; i < size; i++) {
        if (data[i] > threshold) {
            out_batch->selection_vector[match_count++] = (uint16_t)i;
        }
    }

    out_batch->size = match_count;
    return 0;
}