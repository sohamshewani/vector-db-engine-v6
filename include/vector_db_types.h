#ifndef VECTOR_DB_TYPES_H
#define VECTOR_DB_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

#define VEC_DB_CHUNK_ROWS 65536
#define VEC_DB_CACHE_ALIGNMENT 64
#define VEC_DB_VECTOR_SIZE 1024

typedef enum {
    VEC_TYPE_INT32 = 0,
    VEC_TYPE_INT64 = 1,
    VEC_TYPE_FLOAT32 = 2,
    VEC_TYPE_FLOAT64 = 3,
    VEC_TYPE_STRING = 4,
    VEC_TYPE_BOOL = 5,
    VEC_TYPE_VECTOR_FP32 = 6
} VectorDataType;

typedef enum {
    COMPRESSION_NONE = 0,
    COMPRESSION_DICTIONARY = 1,
    COMPRESSION_RLE = 2,
    COMPRESSION_ROARING = 3
} CompressionType;

typedef enum {
    ROARING_CONTAINER_ARRAY = 0,
    ROARING_CONTAINER_BITMAP = 1,
    ROARING_CONTAINER_RUN = 2
} RoaringContainerType;

typedef struct {
    RoaringContainerType type;
    uint16_t card;
    uint16_t capacity;
    void* data;
} RoaringContainer;

typedef struct {
    uint32_t num_containers;
    uint32_t* keys;
    RoaringContainer* containers;
} BitmapIndex;

typedef struct {
    uint32_t dict_size;
    uint32_t value_capacity;
    void* encoded_keys;
    char* string_data;
    uint32_t* offsets;
} DictionaryEncoding;

typedef struct {
    uint32_t num_runs;
    uint32_t capacity;
    uint32_t* run_lengths;
    uint32_t* run_values;
} RLEEncoding;

typedef struct {
    uint32_t row_count;
    uint32_t null_count;
    CompressionType compression;
    uint32_t crc32;
    bool is_sealed;
    uint8_t reserved[3];
    uint64_t null_bitmap[(VEC_DB_CHUNK_ROWS + 63) / 64];
    union {
        DictionaryEncoding dict;
        RLEEncoding rle;
        BitmapIndex roaring;
        struct {
            void* raw_data;
            size_t data_size;
        } uncompressed;
    } storage;
} ColumnChunk;

typedef struct {
    uint32_t capacity;
    uint32_t size;
    uint16_t indices[VEC_DB_VECTOR_SIZE];
} SelectionVector;

typedef struct {
    uint32_t batch_size;
    void* columns[16];
    uint64_t* validity_masks[16];
    SelectionVector sel_vector;
    bool has_selection;
    uint8_t column_count;
    uint8_t reserved[6];
} VectorBatch;

typedef struct {
    atomic_uint state;
    atomic_int readers;
    atomic_int exclusive_waiters;
} RWLatch;

typedef struct {
    size_t capacity;
    size_t offset;
    uint8_t* memory;
    bool use_huge_pages;
    struct MemoryArena* next;
} MemoryArena;

typedef struct {
    MemoryArena* current_arena;
    MemoryArena* arena_list;
    size_t total_allocated;
} ThreadArenaAllocator;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t lsn;
    uint32_t checksum;
    uint32_t transaction_id;
    uint32_t operation_type;
    uint32_t payload_size;
} WALHeader;

#endif // VECTOR_DB_TYPES_H