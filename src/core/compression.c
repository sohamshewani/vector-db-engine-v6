#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <immintrin.h>
#include <errno.h>

#define CHUNK_SIZE 65536
#define ALIGNMENT 64

typedef enum {
    COMPRESSION_NONE = 0,
    COMPRESSION_RLE = 1,
    COMPRESSION_DICTIONARY = 2,
    COMPRESSION_BITPACKED = 3
} CompressionType;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t row_count;
    uint32_t compression_type;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t crc32;
    uint32_t reserved;
} ColumnChunkHeader;

typedef struct {
    ColumnChunkHeader header;
    uint8_t *data;
} ColumnChunk;

static uint32_t calculate_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (-(int)(crc & 1) & 0xEDB88320);
        }
    }
    return ~crc;
}

static void* aligned_alloc_wrapper(size_t size) {
    void *ptr = NULL;
    if (posix_memalign(&ptr, ALIGNMENT, size) != 0) {
        return NULL;
    }
    return ptr;
}

static void aligned_free_wrapper(void *ptr) {
    free(ptr);
}

int compress_rle_int32(const int32_t *src, uint32_t row_count, uint8_t *dest, size_t dest_capacity, size_t *out_size) {
    if (row_count == 0) {
        *out_size = 0;
        return 0;
    }

    size_t offset = 0;
    uint32_t i = 0;

    while (i < row_count) {
        int32_t val = src[i];
        uint32_t run_length = 1;

        while ((i + run_length < row_count) && (src[i + run_length] == val) && (run_length < UINT32_MAX)) {
            run_length++;
        }

        if (offset + sizeof(int32_t) + sizeof(uint32_t) > dest_capacity) {
            return -ENOMEM;
        }

        memcpy(dest + offset, &val, sizeof(int32_t));
        offset += sizeof(int32_t);
        memcpy(dest + offset, &run_length, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        i += run_length;
    }

    *out_size = offset;
    return 0;
}

int decompress_rle_int32(const uint8_t *src, size_t src_size, int32_t *dest, uint32_t row_count) {
    size_t offset = 0;
    uint32_t decoded_rows = 0;

    while (offset < src_size && decoded_rows < row_count) {
        if (offset + sizeof(int32_t) + sizeof(uint32_t) > src_size) {
            return -EINVAL;
        }

        int32_t val;
        uint32_t run_length;

        memcpy(&val, src + offset, sizeof(int32_t));
        offset += sizeof(int32_t);
        memcpy(&run_length, src + offset, sizeof(uint32_t));
        offset += sizeof(uint32_t);

        if (decoded_rows + run_length > row_count) {
            return -EINVAL;
        }

        for (uint32_t i = 0; i < run_length; i++) {
            dest[decoded_rows++] = val;
        }
    }

    if (decoded_rows != row_count) {
        return -EINVAL;
    }

    return 0;
}

int compress_column_chunk(const int32_t *src_data, uint32_t row_count, CompressionType type, ColumnChunk *out_chunk) {
    if (!src_data || row_count == 0 || row_count > CHUNK_SIZE || !out_chunk) {
        return -EINVAL;
    }

    size_t max_dest_size = row_count * (sizeof(int32_t) + sizeof(uint32_t)) + ALIGNMENT;
    uint8_t *temp_buf = (uint8_t *)aligned_alloc_wrapper(max_dest_size);
    if (!temp_buf) {
        return -ENOMEM;
    }

    size_t compressed_size = 0;
    int ret = 0;

    switch (type) {
        case COMPRESSION_NONE:
            compressed_size = row_count * sizeof(int32_t);
            if (max_dest_size < compressed_size) {
                aligned_free_wrapper(temp_buf);
                return -ENOMEM;
            }
            memcpy(temp_buf, src_data, compressed_size);
            break;

        case COMPRESSION_RLE:
            ret = compress_rle_int32(src_data, row_count, temp_buf, max_dest_size, &compressed_size);
            if (ret != 0) {
                aligned_free_wrapper(temp_buf);
                return ret;
            }
            break;

        case COMPRESSION_DICTIONARY:
        case COMPRESSION_BITPACKED:
            {
                compressed_size = row_count * sizeof(int32_t);
                memcpy(temp_buf, src_data, compressed_size);
                type = COMPRESSION_NONE;
            }
            break;

        default:
            aligned_free_wrapper(temp_buf);
            return -EINVAL;
    }

    uint8_t *final_buf = (uint8_t *)aligned_alloc_wrapper(compressed_size);
    if (!final_buf) {
        aligned_free_wrapper(temp_buf);
        return -ENOMEM;
    }

    memcpy(final_buf, temp_buf, compressed_size);
    aligned_free_wrapper(temp_buf);

    out_chunk->header.magic = 0x56434442; 
    out_chunk->header.version = 1;
    out_chunk->header.row_count = row_count;
    out_chunk->header.compression_type = (uint32_t)type;
    out_chunk->header.compressed_size = (uint32_t)compressed_size;
    out_chunk->header.uncompressed_size = row_count * (uint32_t)sizeof(int32_t);
    out_chunk->header.crc32 = calculate_crc32(final_buf, compressed_size);
    out_chunk->header.reserved = 0;
    out_chunk->data = final_buf;

    return 0;
}

int decompress_column_chunk(const ColumnChunk *chunk, int32_t *dest_data) {
    if (!chunk || !dest_data || !chunk->data) {
        return -EINVAL;
    }

    uint32_t computed_crc = calculate_crc32(chunk->data, chunk->header.compressed_size);
    if (computed_crc != chunk->header.crc32) {
        return -EIO; 
    }

    switch (chunk->header.compression_type) {
        case COMPRESSION_NONE:
            if (chunk->header.compressed_size != chunk->header.row_count * sizeof(int32_t)) {
                return -EINVAL;
            }
            memcpy(dest_data, chunk->data, chunk->header.compressed_size);
            return 0;

        case COMPRESSION_RLE:
            return decompress_rle_int32(chunk->data, chunk->header.compressed_size, dest_data, chunk->header.row_count);

        default:
            return -ENOTSUP;
    }
}

void free_column_chunk(ColumnChunk *chunk) {
    if (chunk) {
        if (chunk->data) {
            aligned_free_wrapper(chunk->data);
            chunk->data = NULL;
        }
        chunk->header.row_count = 0;
        chunk->header.compressed_size = 0;
        chunk->header.uncompressed_size = 0;
    }
}

void vectorized_filter_gt_scalar(const int32_t *data, uint32_t row_count, int32_t threshold, uint16_t *selection_vector, uint32_t *out_count) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < row_count; i++) {
        if (data[i] > threshold) {
            selection_vector[count++] = (uint16_t)i;
        }
    }
    *out_count = count;
}

void vectorized_filter_gt_avx512(const int32_t *data, uint32_t row_count, int32_t threshold, uint16_t *selection_vector, uint32_t *out_count) {
    uint32_t count = 0;
    uint32_t i = 0;
    __m512i thresh_vec = _mm512_set1_epi32(threshold);

    for (; i + 16 <= row_count; i += 16) {
        const int32_t *ptr = data + i;
        __m512i chunk = _mm512_loadu_si512((const __m512i *)ptr);
        __mmask16 mask = _mm512_cmpgt_epi32_mask(chunk, thresh_vec);

        while (mask != 0) {
            unsigned long index;
            #if defined(_MSC_VER)
            _BitScanForward(&index, mask);
            #else
            index = __builtin_ctz(mask);
            #endif
            selection_vector[count++] = (uint16_t)(i + index);
            mask &= ~(1U << index);
        }
    }

    for (; i < row_count; i++) {
        if (data[i] > threshold) {
            selection_vector[count++] = (uint16_t)i;
        }
    }

    *out_count = count;
}