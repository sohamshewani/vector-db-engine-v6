#define _GNU_SOURCE
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <errno.h>
#include <pthread.h>

#define ARENA_ALIGNMENT 64
#define ARENA_HUGE_PAGE_SIZE (2UL * 1024 * 1024)

typedef struct ArenaBlock {
    uint8_t *base;
    size_t capacity;
    size_t offset;
    struct ArenaBlock *next;
} ArenaBlock;

struct MemoryArena {
    ArenaBlock *current_block;
    size_t default_block_size;
    bool use_huge_pages;
    pthread_mutex_t lock;
    atomic_size_t total_allocated;
};

typedef struct MemoryArena MemoryArena;

static inline size_t align_forward(size_t ptr, size_t alignment) {
    size_t modulo = ptr & (alignment - 1);
    if (modulo == 0) {
        return ptr;
    }
    return ptr + (alignment - modulo);
}

static ArenaBlock *arena_block_create(size_t size, bool use_huge_pages) {
    size_t aligned_size = align_forward(size, ARENA_ALIGNMENT);
    
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (use_huge_pages) {
#ifdef MAP_HUGETLB
        flags |= MAP_HUGETLB;
#endif
    }

    void *addr = mmap(NULL, aligned_size, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (addr == MAP_FAILED) {
        if (use_huge_pages) {
            flags &= ~MAP_HUGETLB;
            addr = mmap(NULL, aligned_size, PROT_READ | PROT_WRITE, flags, -1, 0);
            if (addr == MAP_FAILED) {
                return NULL;
            }
        } else {
            return NULL;
        }
    }

#ifdef MADV_HUGEPAGE
    if (!use_huge_pages) {
        madvise(addr, aligned_size, MADV_HUGEPAGE);
    }
#endif

#ifdef MADV_WILLNEED
    madvise(addr, aligned_size, MADV_WILLNEED);
#endif

    ArenaBlock *block = (ArenaBlock *)malloc(sizeof(ArenaBlock));
    if (!block) {
        munmap(addr, aligned_size);
        return NULL;
    }

    block->base = (uint8_t *)addr;
    block->capacity = aligned_size;
    block->offset = 0;
    block->next = NULL;

    return block;
}

static void arena_block_destroy(ArenaBlock *block) {
    if (!block) {
        return;
    }
    munmap(block->base, block->capacity);
    free(block);
}

MemoryArena *arena_create(size_t default_block_size, bool use_huge_pages) {
    MemoryArena *arena = (MemoryArena *)malloc(sizeof(MemoryArena));
    if (!arena) {
        return NULL;
    }

    if (default_block_size < ARENA_HUGE_PAGE_SIZE && use_huge_pages) {
        default_block_size = ARENA_HUGE_PAGE_SIZE;
    }

    ArenaBlock *first_block = arena_block_create(default_block_size, use_huge_pages);
    if (!first_block) {
        free(arena);
        return NULL;
    }

    arena->current_block = first_block;
    arena->default_block_size = default_block_size;
    arena->use_huge_pages = use_huge_pages;
    atomic_init(&arena->total_allocated, first_block->capacity);

    if (pthread_mutex_init(&arena->lock, NULL) != 0) {
        arena_block_destroy(first_block);
        free(arena);
        return NULL;
    }

    return arena;
}

void arena_destroy(MemoryArena *arena) {
    if (!arena) {
        return;
    }

    pthread_mutex_lock(&arena->lock);
    ArenaBlock *current = arena->current_block;
    while (current) {
        ArenaBlock *next = current->next;
        arena_block_destroy(current);
        current = next;
    }
    pthread_mutex_unlock(&arena->lock);

    pthread_mutex_destroy(&arena->lock);
    free(arena);
}

void *arena_alloc(MemoryArena *arena, size_t size) {
    if (!arena || size == 0) {
        return NULL;
    }

    size_t aligned_size = align_forward(size, ARENA_ALIGNMENT);

    pthread_mutex_lock(&arena->lock);

    ArenaBlock *block = arena->current_block;
    size_t current_offset = align_forward((size_t)(block->base + block->offset), ARENA_ALIGNMENT) - (size_t)block->base;

    if (current_offset + aligned_size > block->capacity) {
        size_t new_block_size = arena->default_block_size;
        if (aligned_size > new_block_size) {
            new_block_size = aligned_size + ARENA_ALIGNMENT;
        }

        ArenaBlock *new_block = arena_block_create(new_block_size, arena->use_huge_pages);
        if (!new_block) {
            pthread_mutex_unlock(&arena->lock);
            return NULL;
        }

        new_block->next = arena->current_block;
        arena->current_block = new_block;
        block = new_block;
        current_offset = align_forward((size_t)(block->base + block->offset), ARENA_ALIGNMENT) - (size_t)block->base;

        atomic_fetch_add(&arena->total_allocated, new_block->capacity);
    }

    void *ptr = (void *)(block->base + current_offset);
    block->offset = current_offset + aligned_size;

    pthread_mutex_unlock(&arena->lock);

    memset(ptr, 0, size);
    return ptr;
}

void *arena_realloc(MemoryArena *arena, void *ptr, size_t old_size, size_t new_size) {
    if (!arena || new_size == 0) {
        return NULL;
    }
    if (!ptr) {
        return arena_alloc(arena, new_size);
    }
    if (new_size <= old_size) {
        return ptr;
    }

    pthread_mutex_lock(&arena->lock);
    ArenaBlock *block = arena->current_block;
    uint8_t *p_bytes = (uint8_t *)ptr;

    if (p_bytes >= block->base && p_bytes < block->base + block->capacity) {
        size_t offset_in_block = (size_t)(p_bytes - block->base);
        if (offset_in_block + old_size == block->offset) {
            size_t additional = align_forward(new_size - old_size, ARENA_ALIGNMENT);
            if (block->offset + additional <= block->capacity) {
                block->offset += additional;
                pthread_mutex_unlock(&arena->lock);
                return ptr;
            }
        }
    }
    pthread_mutex_unlock(&arena->lock);

    void *new_ptr = arena_alloc(arena, new_size);
    if (!new_ptr) {
        return NULL;
    }

    memcpy(new_ptr, ptr, old_size);
    return new_ptr;
}

void arena_reset(MemoryArena *arena) {
    if (!arena) {
        return;
    }

    pthread_mutex_lock(&arena->lock);

    ArenaBlock *current = arena->current_block->next;
    while (current) {
        ArenaBlock *next = current->next;
        arena_block_destroy(current);
        current = next;
    }

    arena->current_block->next = NULL;
    arena->current_block->offset = 0;
    atomic_store(&arena->total_allocated, arena->current_block->capacity);

    pthread_mutex_unlock(&arena->lock);
}

size_t arena_total_allocated(MemoryArena *arena) {
    if (!arena) {
        return 0;
    }
    return atomic_load(&arena->total_allocated);
}