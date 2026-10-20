#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <time.h>

/* ========================================================================= *
 * VECTOR DB ENGINE IMPLEMENTATION (Embedded for self-contained testing)      *
 * ========================================================================= */

#define VDB_DIM 16
#define VDB_MAX_ELEMENTS 1024
#define VDB_HSNW_M 16
#define VDB_HSNW_EF 32

typedef struct {
    uint64_t id;
    float data[VDB_DIM];
    int active;
} vdb_vector_t;

typedef struct {
    vdb_vector_t elements[VDB_MAX_ELEMENTS];
    size_t count;
    pthread_rwlock_t rwlock;
    int corrupted;
} vdb_engine_t;

static inline float vdb_cosine_distance(const float *a, const float *b) {
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    for (int i = 0; i < VDB_DIM; i++) {
        dot += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }
    if (norm_a == 0.0f || norm_b == 0.0f) return 1.0f;
    float sim = dot / (sqrtf(norm_a) * sqrtf(norm_b));
    if (sim > 1.0f) sim = 1.0f;
    if (sim < -1.0f) sim = -1.0f;
    return 1.0f - sim;
}

vdb_engine_t* vdb_create(void) {
    vdb_engine_t *db = (vdb_engine_t*)malloc(sizeof(vdb_engine_t));
    if (!db) return NULL;
    memset(db, 0, sizeof(vdb_engine_t));
    if (pthread_rwlock_init(&db->rwlock, NULL) != 0) {
        free(db);
        return NULL;
    }
    return db;
}

void vdb_destroy(vdb_engine_t *db) {
    if (!db) return;
    pthread_rwlock_wrlock(&db->rwlock);
    pthread_rwlock_unlock(&db->rwlock);
    pthread_rwlock_destroy(&db->rwlock);
    free(db);
}

int vdb_insert(vdb_engine_t *db, uint64_t id, const float *data) {
    if (!db || !data) return EINVAL;
    pthread_rwlock_wrlock(&db->rwlock);
    if (db->corrupted) {
        pthread_rwlock_unlock(&db->rwlock);
        return EFAULT;
    }
    if (db->count >= VDB_MAX_ELEMENTS) {
        pthread_rwlock_unlock(&db->rwlock);
        return ENOMEM;
    }
    for (size_t i = 0; i < db->count; i++) {
        if (db->elements[i].active && db->elements[i].id == id) {
            pthread_rwlock_unlock(&db->rwlock);
            return EEXIST;
        }
    }
    size_t idx = db->count++;
    db->elements[idx].id = id;
    memcpy(db->elements[idx].data, data, sizeof(float) * VDB_DIM);
    db->elements[idx].active = 1;
    pthread_rwlock_unlock(&db->rwlock);
    return 0;
}

int vdb_get(vdb_engine_t *db, uint64_t id, float *out_data) {
    if (!db || !out_data) return EINVAL;
    pthread_rwlock_rdlock(&db->rwlock);
    if (db->corrupted) {
        pthread_rwlock_unlock(&db->rwlock);
        return EFAULT;
    }
    for (size_t i = 0; i < db->count; i++) {
        if (db->elements[i].active && db->elements[i].id == id) {
            memcpy(out_data, db->elements[i].data, sizeof(float) * VDB_DIM);
            pthread_rwlock_unlock(&db->rwlock);
            return 0;
        }
    }
    pthread_rwlock_unlock(&db->rwlock);
    return ENOENT;
}

int vdb_delete(vdb_engine_t *db, uint64_t id) {
    if (!db) return EINVAL;
    pthread_rwlock_wrlock(&db->rwlock);
    if (db->corrupted) {
        pthread_rwlock_unlock(&db->rwlock);
        return EFAULT;
    }
    for (size_t i = 0; i < db->count; i++) {
        if (db->elements[i].active && db->elements[i].id == id) {
            db->elements[i].active = 0;
            pthread_rwlock_unlock(&db->rwlock);
            return 0;
        }
    }
    pthread_rwlock_unlock(&db->rwlock);
    return ENOENT;
}

int vdb_search(vdb_engine_t *db, const float *query, uint64_t *out_id, float *out_dist) {
    if (!db || !query || !out_id || !out_dist) return EINVAL;
    pthread_rwlock_rdlock(&db->rwlock);
    if (db->corrupted) {
        pthread_rwlock_unlock(&db->rwlock);
        return EFAULT;
    }
    float min_dist = 1e9f;
    uint64_t best_id = 0;
    int found = 0;
    for (size_t i = 0; i < db->count; i++) {
        if (!db->elements[i].active) continue;
        float d = vdb_cosine_distance(query, db->elements[i].data);
        if (d < min_dist) {
            min_dist = d;
            best_id = db->elements[i].id;
            found = 1;
        }
    }
    pthread_rwlock_unlock(&db->rwlock);
    if (!found) return ENOENT;
    *out_id = best_id;
    *out_dist = min_dist;
    return 0;
}

size_t vdb_count(vdb_engine_t *db) {
    pthread_rwlock_rdlock(&db->rwlock);
    size_t active_count = 0;
    for (size_t i = 0; i < db->count; i++) {
        if (db->elements[i].active) active_count++;
    }
    pthread_rwlock_unlock(&db->rwlock);
    return active_count;
}

void vdb_force_corrupt(vdb_engine_t *db) {
    pthread_rwlock_wrlock(&db->rwlock);
    db->corrupted = 1;
    pthread_rwlock_unlock(&db->rwlock);
}

void vdb_recover(vdb_engine_t *db) {
    pthread_rwlock_wrlock(&db->rwlock);
    db->corrupted = 0;
    db->count = 0;
    memset(db->elements, 0, sizeof(db->elements));
    pthread_rwlock_unlock(&db->rwlock);
}

/* ========================================================================= *
 * TEST FRAMEWORK & SUITE                                                   *
 * ========================================================================= */

#define RUN_TEST(test_func) do { \
    printf("[ RUN      ] %s\n", #test_func); \
    test_func(); \
    printf("[       OK ] %s\n", #test_func); \
} while(0)

static void fill_vector(float *vec, float seed) {
    for (int i = 0; i < VDB_DIM; i++) {
        vec[i] = sinf(seed + (float)i);
    }
}

/* 1. Unit & Boundary Tests */
static void test_unit_boundaries(void) {
    vdb_engine_t *db = vdb_create();
    assert(db != NULL);

    float vec[VDB_DIM];
    fill_vector(vec, 1.0f);

    /* Invalid arguments */
    assert(vdb_insert(NULL, 1, vec) == EINVAL);
    assert(vdb_insert(db, 1, NULL) == EINVAL);
    assert(vdb_get(db, 1, NULL) == EINVAL);
    assert(vdb_get(NULL, 1, vec) == EINVAL);
    assert(vdb_delete(NULL, 1) == EINVAL);
    
    uint64_t out_id;
    float out_dist;
    assert(vdb_search(db, NULL, &out_id, &out_dist) == EINVAL);
    assert(vdb_search(db, vec, NULL, &out_dist) == EINVAL);
    assert(vdb_search(db, vec, &out_id, NULL) == EINVAL);

    /* Not found & Empty DB behavior */
    assert(vdb_get(db, 999, vec) == ENOENT);
    assert(vdb_delete(db, 999) == ENOENT);
    assert(vdb_search(db, vec, &out_id, &out_dist) == ENOENT);
    assert(vdb_count(db) == 0);

    /* Successful insert and retrieval */
    assert(vdb_insert(db, 42, vec) == 0);
    assert(vdb_count(db) == 1);
    
    float retrieved[VDB_DIM];
    assert(vdb_get(db, 42, retrieved) == 0);
    for (int i = 0; i < VDB_DIM; i++) {
        assert(retrieved[i] == vec[i]);
    }

    /* Duplicate insertion check */
    assert(vdb_insert(db, 42, vec) == EEXIST);

    /* Search verification */
    assert(vdb_search(db, vec, &out_id, &out_dist) == 0);
    assert(out_id == 42);
    assert(out_dist < 1e-5f);

    /* Deletion */
    assert(vdb_delete(db, 42) == 0);
    assert(vdb_count(db) == 0);
    assert(vdb_get(db, 42, retrieved) == ENOENT);

    vdb_destroy(db);
}

/* 2. Invariant Tests */
static void test_invariants_capacity(void) {
    vdb_engine_t *db = vdb_create();
    assert(db != NULL);

    float vec[VDB_DIM];
    for (size_t i = 0; i < VDB_MAX_ELEMENTS; i++) {
        fill_vector(vec, (float)i);
        assert(vdb_insert(db, (uint64_t)i, vec) == 0);
    }
    assert(vdb_count(db) == VDB_MAX_ELEMENTS);

    /* Exceed capacity */
    fill_vector(vec, 9999.0f);
    assert(vdb_insert(db, 9999, vec) == ENOMEM);

    /* Invariant: Capacity limits enforced, count stable */
    assert(vdb_count(db) == VDB_MAX_ELEMENTS);

    vdb_destroy(db);
}

/* 3. Concurrent Stress & Contention Tests */
typedef struct {
    vdb_engine_t *db;
    int thread_id;
    int iterations;
    int *error_counter;
} thread_arg