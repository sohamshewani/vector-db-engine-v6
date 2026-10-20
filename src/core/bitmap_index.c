#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#define VECTOR_DB_CACHE_LINE 64
#define BITMAP_CONTAINER_CAPACITY 1024
#define RUN_CONTAINER_MAX_RUNS 4096
#define ARRAY_CONTAINER_MAX_VALS 4096

typedef enum {
    CONTAINER_ARRAY = 0,
    CONTAINER_BITMAP = 1,
    CONTAINER_RUN = 2
} ContainerType;

typedef struct {
    ContainerType type;
    uint16_t cardinality;
    uint16_t capacity;
    void* data;
} BitmapContainer;

typedef struct {
    uint32_t val;
    uint32_t len;
} RunElement;

typedef struct {
    uint32_t num_chunks;
    uint32_t max_chunks;
    uint16_t* keys;
    BitmapContainer** containers;
} BitmapIndex;

static inline void* db_aligned_alloc(size_t alignment, size_t size) {
    void* ptr = NULL;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return NULL;
    }
    return ptr;
}

static inline void db_aligned_free(void* ptr) {
    free(ptr);
}

BitmapIndex* bitmap_index_create(void) {
    BitmapIndex* index = (BitmapIndex*)db_aligned_alloc(VECTOR_DB_CACHE_LINE, sizeof(BitmapIndex));
    if (!index) return NULL;

    index->num_chunks = 0;
    index->max_chunks = 16;
    index->keys = (uint16_t*)db_aligned_alloc(VECTOR_DB_CACHE_LINE, index->max_chunks * sizeof(uint16_t));
    index->containers = (BitmapContainer**)db_aligned_alloc(VECTOR_DB_CACHE_LINE, index->max_chunks * sizeof(BitmapContainer*));

    if (!index->keys || !index->containers) {
        db_aligned_free(index->keys);
        db_aligned_free(index->containers);
        db_aligned_free(index);
        return NULL;
    }

    memset(index->keys, 0, index->max_chunks * sizeof(uint16_t));
    memset(index->containers, 0, index->max_chunks * sizeof(BitmapContainer*));
    return index;
}

static BitmapContainer* container_create(ContainerType type) {
    BitmapContainer* c = (BitmapContainer*)db_aligned_alloc(VECTOR_DB_CACHE_LINE, sizeof(BitmapContainer));
    if (!c) return NULL;

    c->type = type;
    c->cardinality = 0;

    if (type == CONTAINER_ARRAY) {
        c->capacity = ARRAY_CONTAINER_MAX_VALS;
        c->data = db_aligned_alloc(VECTOR_DB_CACHE_LINE, c->capacity * sizeof(uint16_t));
    } else if (type == CONTAINER_BITMAP) {
        c->capacity = BITMAP_CONTAINER_CAPACITY;
        c->data = db_aligned_alloc(VECTOR_DB_CACHE_LINE, c->capacity * sizeof(uint64_t));
        memset(c->data, 0, c->capacity * sizeof(uint64_t));
    } else if (type == CONTAINER_RUN) {
        c->capacity = RUN_CONTAINER_MAX_RUNS;
        c->data = db_aligned_alloc(VECTOR_DB_CACHE_LINE, c->capacity * sizeof(RunElement));
    }

    if (!c->data) {
        db_aligned_free(c);
        return NULL;
    }
    return c;
}

static void container_destroy(BitmapContainer* c) {
    if (!c) return;
    db_aligned_free(c->data);
    db_aligned_free(c);
}

void bitmap_index_destroy(BitmapIndex* index) {
    if (!index) return;
    for (uint32_t i = 0; i < index->num_chunks; i++) {
        container_destroy(index->containers[i]);
    }
    db_aligned_free(index->keys);
    db_aligned_free(index->containers);
    db_aligned_free(index);
}

static int container_array_add(BitmapContainer* c, uint16_t val) {
    uint16_t* arr = (uint16_t*)c->data;
    for (uint32_t i = 0; i < c->cardinality; i++) {
        if (arr[i] == val) return 0;
        if (arr[i] > val) {
            if (c->cardinality >= c->capacity) return -1;
            memmove(&arr[i + 1], &arr[i], (c->cardinality - i) * sizeof(uint16_t));
            arr[i] = val;
            c->cardinality++;
            return 1;
        }
    }
    if (c->cardinality >= c->capacity) return -1;
    arr[c->cardinality++] = val;
    return 1;
}

static BitmapContainer* container_upgrade_array_to_bitmap(BitmapContainer* c) {
    BitmapContainer* b = container_create(CONTAINER_BITMAP);
    if (!b) return NULL;

    uint16_t* arr = (uint16_t*)c->data;
    uint64_t* bits = (uint64_t*)b->data;

    for (uint32_t i = 0; i < c->cardinality; i++) {
        uint16_t val = arr[i];
        uint32_t word_idx = val >> 6;
        uint32_t bit_idx = val & 63;
        bits[word_idx] |= (1ULL << bit_idx);
    }
    b->cardinality = c->cardinality;
    container_destroy(c);
    return b;
}

int bitmap_index_add(BitmapIndex* index, uint16_t chunk_key, uint16_t row_id) {
    if (!index) return -1;

    uint32_t target_idx = index->num_chunks;
    bool found = false;

    for (uint32_t i = 0; i < index->num_chunks; i++) {
        if (index->keys[i] == chunk_key) {
            target_idx = i;
            found = true;
            break;
        }
    }

    if (!found) {
        if (index->num_chunks >= index->max_chunks) {
            uint32_t new_max = index->max_chunks * 2;
            uint16_t* new_keys = (uint16_t*)db_aligned_alloc(VECTOR_DB_CACHE_LINE, new_max * sizeof(uint16_t));
            BitmapContainer** new_containers = (BitmapContainer**)db_aligned_alloc(VECTOR_DB_CACHE_LINE, new_max * sizeof(BitmapContainer*));
            if (!new_keys || !new_containers) {
                db_aligned_free(new_keys);
                db_aligned_free(new_containers);
                return -1;
            }
            memcpy(new_keys, index->keys, index->num_chunks * sizeof(uint16_t));
            memcpy(new_containers, index->containers, index->num_chunks * sizeof(BitmapContainer*));
            db_aligned_free(index->keys);
            db_aligned_free(index->containers);
            index->keys = new_keys;
            index->containers = new_containers;
            index->max_chunks = new_max;
        }

        index->keys[index->num_chunks] = chunk_key;
        index->containers[index->num_chunks] = container_create(CONTAINER_ARRAY);
        if (!index->containers[index->num_chunks]) return -1;
        target_idx = index->num_chunks;
        index->num_chunks++;
    }

    BitmapContainer* c = index->containers[target_idx];
    if (c->type == CONTAINER_ARRAY) {
        int res = container_array_add(c, row_id);
        if (res < 0) {
            BitmapContainer* b = container_upgrade_array_to_bitmap(c);
            if (!b) return -1;
            index->containers[target_idx] = b;
            c = b;
            uint32_t word_idx = row_id >> 6;
            uint32_t bit_idx = row_id & 63;
            uint64_t* bits = (uint64_t*)c->data;
            if (!(bits[word_idx] & (1ULL << bit_idx))) {
                bits[word_idx] |= (1ULL << bit_idx);
                c->cardinality++;
            }
        }
    } else if (c->type == CONTAINER_BITMAP) {
        uint32_t word_idx = row_id >> 6;
        uint32_t bit_idx = row_id & 63;
        uint64_t* bits = (uint64_t*)c->data;
        if (!(bits[word_idx] & (1ULL << bit_idx))) {
            bits[word_idx] |= (1ULL << bit_idx);
            c->cardinality++;
        }
    } else {
        return -1;
    }

    return 0;
}

static BitmapContainer* container_intersect(BitmapContainer* a, BitmapContainer* b) {
    if (a->type == CONTAINER_ARRAY && b->type == CONTAINER_ARRAY) {
        BitmapContainer* res = container_create(CONTAINER_ARRAY);
        if (!res) return NULL;
        uint16_t* arr_a = (uint16_t*)a->data;
        uint16_t* arr_b = (uint16_t*)b->data;
        uint32_t i = 0, j = 0;
        while (i < a->cardinality && j < b->cardinality) {
            if (arr_a[i] == arr_b[j]) {
                container_array_add(res, arr_a[i]);
                i++;
                j++;
            } else if (arr_a[i] < arr_b[j]) {
                i++;
            } else {
                j++;
            }
        }
        return res;
    } else if (a->type == CONTAINER_BITMAP && b->type == CONTAINER_BITMAP) {
        BitmapContainer* res = container_create(CONTAINER_BITMAP);
        if (!res) return NULL;
        uint64_t* bits_a = (uint64_t*)a->data;
        uint64_t* bits_b = (uint64_t*)b->data;
        uint64_t* bits_res = (uint64_t*)res->data;
        uint32_t card = 0;
        for (uint32_t i = 0; i < BITMAP_CONTAINER_CAPACITY; i++) {
            bits_res[i] = bits_a[i] & bits_b[i];
            card += __builtin_popcountll(bits_res[i]);
        }
        res->cardinality = card;
        return res;
    } else {
        BitmapContainer* res = container_create(CONTAINER_BITMAP);
        if (!res) return NULL;
        uint64_t* bits_res = (uint64_t*)res->data;
        uint32_t card = 0;

        BitmapContainer* arr_c = (a->type == CONTAINER_ARRAY) ? a : b;
        BitmapContainer* bit_c = (a->type == CONTAINER_BITMAP) ? a : b;

        uint16_t* arr = (uint16_t*)arr_c->data;
        uint64_t* bits = (uint64_t*)bit_c->data;

        for (uint32_t i = 0; i < arr_c->cardinality; i++) {
            uint16_t val = arr[i];
            uint32_t word_idx = val >> 6;
            uint32_t bit_idx = val & 63;
            if (bits[word_idx] & (1ULL << bit_idx)) {
                uint32_t w = val >> 6;
                uint32_t b_idx = val & 63;
                if (!(bits_res[w] & (1ULL << b_idx))) {
                    bits_res[w] |= (1ULL << b_idx);
                    card++;
                }
            }
        }
        res->cardinality = card;
        return res;
    }
}

BitmapIndex* bitmap_index_intersect(BitmapIndex* idx1, BitmapIndex* idx2) {
    if (!idx1 || !idx2) return NULL;

    BitmapIndex* result = bitmap_index_create();
    if (!result) return NULL;

    for (uint32_t i = 0; i < idx1->num_chunks; i++) {
        uint16_t key = idx1->keys[i];
        for (uint32_t j = 0; j < idx2->num_chunks; j++) {
            if (idx2->keys[j] == key) {
                BitmapContainer* intersected = container_intersect(idx1->containers[i], idx2->containers[j]);
                if (intersected && intersected->cardinality > 0) {
                    if (result->num_chunks >= result->max_chunks) {
                        uint32_t new_max = result->max_chunks * 2;
                        uint16_t* new_keys = (uint16_t*)db_aligned_alloc(VECTOR_DB_CACHE_LINE, new_max * sizeof(uint16_t));
                        BitmapContainer** new_containers = (BitmapContainer**)db_aligned_alloc(VECTOR_DB_CACHE_LINE, new_max * sizeof(BitmapContainer*));
                        if (!new_keys || !new_containers) {
                            db_aligned_free(new_keys);
                            db_aligned_free(new_containers);
                            bitmap_index_destroy(result);
                            container_destroy(intersected);
                            return NULL;
                        }
                        memcpy(new_keys, result->keys, result->num_chunks * sizeof(uint16_t));
                        memcpy(new_containers, result->containers, result->num_chunks * sizeof(BitmapContainer*));
                        db_aligned_free(result->keys);
                        db_aligned_free(result->containers);
                        result->keys = new_keys;
                        result->containers = new_containers;
                        result->max_chunks = new_max;
                    }
                    result->keys[result->num_chunks] = key;
                    result->containers[result->num_chunks] = intersected;
                    result->num_chunks++;
                } else {
                    container_destroy(intersected);
                }
                break;
            }
        }
    }

    return result;
}

static BitmapContainer* container_union(BitmapContainer* a, BitmapContainer* b) {
    BitmapContainer* res = container_create(CONTAINER_BITMAP);
    if (!res) return NULL;
    uint64_t* bits_res = (uint64_t*)res->data;
    uint32_t card = 0;

    if (a->type == CONTAINER_ARRAY) {
        uint16_t* arr = (uint16_t*)a->data;
        for (uint32_t i = 0; i < a->cardinality; i++) {
            uint16_t val = arr[i];
            bits_res[val >> 6] |= (1ULL << (val & 63));
        }
    } else {
        memcpy(bits_res, a->data, BITMAP_CONTAINER_CAPACITY * sizeof(uint64_t));
    }

    if (b->type == CONTAINER_ARRAY) {
        uint16_t* arr = (uint16_t*)b->data;
        for (uint32_t i = 0; i < b->cardinality; i++) {
            uint16_t val = arr[i];
            bits_res[val