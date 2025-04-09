
#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <time.h>
#include <sys/errno.h>
#include <errno.h>

#include "lab.h"


#define handle_error_and_die(msg) \
    do                            \
    {                             \
        perror(msg);              \
        raise(SIGKILL);          \
    } while (0)

/**
 * Helper fucntion to insert free block into the free list for given k
 */
static void insert_free_block(struct buddy_pool *pool, unsigned int k, struct avail *block){
    block->tag = BLOCK_AVAIL;
    block->kval = k;
    block->next =pool->avail[k].next;
    block->prev = &pool->avail[k];
    if (pool->avail[k].next != &pool->avail[k])
        pool->avail[k].next->prev = block;
    pool->avail[k].next = block;
}


/**
 * Helper function to remove a block from its free list
 */
static void remove_free_block(struct avail *block){
    //adjust pointers
    if(block->prev)
        block->prev->next = block->next;
    if(block-> next)
        block->next->prev = block->prev;
    block->next  = NULL;
    block->prev = NULL;
}

/**
 * @brief Convert bytes to the correct K value
 *
 * @param bytes the number of bytes
 * @return size_t the K value that will fit bytes
 */
size_t btok(size_t bytes)
{
    size_t k =0;
    size_t size = UINT64_C(1);
    while(size < bytes){
        size <<= 1;
        k++;
    }
    if(k < SMALLEST_K)
        k = SMALLEST_K;
    return k;
}


struct avail *buddy_calc(struct buddy_pool *pool, struct avail *buddy)
{
    size_t offset = (size_t)((char *)buddy - (char *)pool->base);
    size_t block_size = UINT64_C(1) << buddy->kval;
    size_t buddy_offset = offset ^ block_size; 
    return (struct avail *)((char *)pool->base + buddy_offset);
}



void *buddy_malloc(struct buddy_pool *pool, size_t size)
{
    //get the kval for the requested size with enough room for the tag and kval fields
    size_t  total_size = size + sizeof(struct avail);
    size_t k = btok(total_size);

    if(k>pool->kval_m){
        errno = ENOMEM;
        return NULL; //requested size is too large, cant handle
    }

    //R1 Find a block
    size_t i;
    for(i = k; i <= pool->kval_m; i++ ){ //searching from the free list
        if(pool->avail[i].next != &pool->avail[i])
            break;
    }
    //There was not enough memory to satisfy the request thus we need to set error and return NULL
    if(i>pool->kval_m){
        errno = ENOMEM;
        return NULL; //No free block available
    }

    //R2 Remove from list;
    struct avail *block = pool->avail[i].next;
    remove_free_block(block);


    //R3 Split required?
    while(i>k){
        i--;
        //R4 Split the block
        struct avail *split_buddy = (struct avail *)((char *)block + (UINT64_C(1) <<i));
        split_buddy->tag = BLOCK_AVAIL;
        split_buddy->kval =i;
        split_buddy->next = NULL;
        split_buddy->prev = NULL;
        insert_free_block(pool, i, split_buddy);
        block->kval =i;
    }
    block->tag = BLOCK_RESERVED;

    /* Return pointer to memory right after the header */
    // return (void *)((char *)block +sizeof(struct avail));
    return (void *)(block + 1);

}

//TODO
void buddy_free(struct buddy_pool *pool, void *ptr)
{
    struct avail *block = (struct avail *)((char *)ptr - sizeof(struct avail));
    block->tag = BLOCK_AVAIL;
    unsigned int k = block->kval;

    // Coalesce with buddy blocks as long as possible.
    while (k < pool->kval_m) {
         struct avail *buddy = buddy_calc(pool, block);
         if (buddy->tag != BLOCK_AVAIL || buddy->kval != k)
             break;
         remove_free_block(buddy);
         if (block > buddy)
             block = buddy;
         k++;
         block->kval = k;
    }
    insert_free_block(pool, k, block);
}


//TODO
/**
 * @brief This is a simple version of realloc.
 *
 * @param poolThe memory pool
 * @param ptr  The user memory
 * @param size the new size requested
 * @return void* pointer to the new user memory
 */
void *buddy_realloc(struct buddy_pool *pool, void *ptr, size_t size)
{
    if(!ptr)
        return buddy_malloc(pool, size);
    if(size ==0){
        buddy_free(pool, ptr);
        return NULL;
    }

    //Retruve the curent block header
    struct avail *block = (struct avail *)((char *)ptr - sizeof(struct avail));
    size_t old_block_size = UINT64_C(1) << block->kval;
    size_t total_size = size + sizeof(struct avail);
    size_t new_k = btok(total_size);

    //if the current block is large enough, rerun it
    if(new_k <= block->kval)
        return ptr;
    
    //Otherwise, allocate a new block and copy over the data
    void *new_ptr = buddy_malloc(pool, size);
    if(!new_ptr){
        errno = ENOMEM;
        return NULL;
    }

    size_t copy_size = old_block_size - sizeof(struct avail);
    if(copy_size >size){
        copy_size = size;
    }
    memcpy(new_ptr, ptr, copy_size);
    buddy_free(pool, ptr);
    return new_ptr;
}

void buddy_init(struct buddy_pool *pool, size_t size)
{
    size_t kval = 0;
    if (size == 0)
        kval = DEFAULT_K;
    else
        kval = btok(size);

    if (kval < MIN_K)
        kval = MIN_K;
    if (kval >= MAX_K)
        kval = MAX_K - 1;

    //make sure pool struct is cleared out
    memset(pool,0,sizeof(struct buddy_pool));

    pool->kval_m = kval;
    pool->numbytes = (UINT64_C(1) << pool->kval_m);
    
    //Memory map a block of raw memory to manage
    pool->base = mmap(
        NULL,                               /*addr to map to*/
        pool->numbytes,                     /*length*/
        PROT_READ | PROT_WRITE,             /*prot*/
        MAP_PRIVATE | MAP_ANONYMOUS,        /*flags*/
        -1,                                 /*fd -1 when using MAP_ANONYMOUS*/
        0                                   /* offset 0 when using MAP_ANONYMOUS*/
    );
    if (MAP_FAILED == pool->base)
    {
        handle_error_and_die("buddy_init avail array mmap failed");
    }

    //Set all blocks to empty. We are using circular lists so the first elements just point
    //to an available block. Thus the tag, and kval feild are unused burning a small bit of
    //memory but making the code more readable. We mark these blocks as UNUSED to aid in debugging.
    for (size_t i = 0; i <= kval; i++)
    {
        pool->avail[i].next = pool->avail[i].prev = &pool->avail[i];
        pool->avail[i].kval = i;
        pool->avail[i].tag = BLOCK_UNUSED;
    }

    //Add in the first block
    pool->avail[kval].next = pool->avail[kval].prev = (struct avail *)pool->base;
    struct avail *m = pool->avail[kval].next;
    m->tag = BLOCK_AVAIL;
    m->kval = kval;
    m->next = m->prev = &pool->avail[kval];
}


//Inverse of buddy_init.
void buddy_destroy(struct buddy_pool *pool)
{
    int rval = munmap(pool->base, pool->numbytes);
    if (-1 == rval)
    {
        handle_error_and_die("buddy_destroy avail array");
    }
    //Zero out the array so it can be reused it needed
    memset(pool, 0, sizeof(struct buddy_pool));
}

#define UNUSED(x) (void)x

/**
 * This function can be useful to visualize the bits in a block. This can
 * help when figuring out the buddy_calc function!
 */
// static void printb(unsigned long int b)
// {
//      size_t bits = sizeof(b) * 8;
//      unsigned long int curr = UINT64_C(1) << (bits - 1);
//      for (size_t i = 0; i < bits; i++)
//      {
//           if (b & curr)
//           {
//                printf("1");
//           }
//           else
//           {
//                printf("0");
//           }
//           curr >>= 1L;
//      }
// }
