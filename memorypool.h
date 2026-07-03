/*
Copyright (c) 2026 Carlos de Diego

This Source Code Form is subject to the terms of the
Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#pragma once

#include <cstdint>
#include <new>
#include <type_traits>
#include "lib/parallel_hashmap/btree.h"

#define IS_64BIT (UINTPTR_MAX > UINT32_MAX)

#if defined(_MSC_VER)
    #define FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    #define FORCE_INLINE inline __attribute__((always_inline))
#else
    #define FORCE_INLINE inline
#endif

namespace mempool {

    // Undefined behaviour when value == 0
    FORCE_INLINE size_t unsafe_bit_scan_forward(const size_t value) {
#if defined(_MSC_VER)
        unsigned long result;
    #if IS_64BIT
        _BitScanForward64(&result, value);
    #else
        _BitScanForward(&result, value);
    #endif
        return result;
#elif defined(__GNUC__) || defined(__clang__)
    #if IS_64BIT
        return __builtin_ctzll(value);
    #else
        return __builtin_ctz(value);
    #endif
#else
    #if IS_64BIT
        const unsigned int tab64[64] = {
             0,  1,  2, 53,  3,  7, 54, 27,
             4, 38, 41,  8, 34, 55, 48, 28,
            62,  5, 39, 46, 44, 42, 22,  9,
            24, 35, 59, 56, 49, 18, 29, 11,
            63, 52,  6, 26, 37, 40, 33, 47,
            61, 45, 43, 21, 23, 58, 17, 10,
            51, 25, 36, 32, 60, 20, 57, 16,
            50, 31, 19, 15, 30, 14, 13, 12
        };
        return tab64[(value & (0 - value)) * 0x022FDD63CC95386D >> 58];
    #else
        static uint8_t tab32[32] = {
             0,  1, 28,  2, 29, 14, 24,  3,
            30, 22, 20, 15, 25, 17,  4,  8,
            31, 27, 13, 23, 21, 19, 16,  7,
            26, 12, 18,  6, 11,  5, 10,  9
        };
        return tab32[(value & (0 - value)) * 0x077CB531U >> 27];
    #endif
#endif
    }

    FORCE_INLINE size_t unsafe_int_log2(size_t value) {
#if defined(_MSC_VER)
        unsigned long result;
    #if IS_64BIT
        _BitScanReverse64(&result, value);
    #else
        _BitScanReverse(&result, value);
    #endif
        return result;
#elif defined(__GNUC__) || defined(__clang__)
    #if IS_64BIT
        return 63 - __builtin_clzll(value);
    #else
        return 31 - __builtin_clz(value);
    #endif
#else
    #if IS_64BIT
        const uint8_t tab64[64] = {
             0, 58,  1, 59, 47, 53,  2, 60,
            39, 48, 27, 54, 33, 42,  3, 61,
            51, 37, 40, 49, 18, 28, 20, 55,
            30, 34, 11, 43, 14, 22,  4, 62,
            57, 46, 52, 38, 26, 32, 41, 50,
            36, 17, 19, 29, 10, 13, 21, 56,
            45, 25, 31, 35, 16,  9, 12, 44,
            24, 15,  8, 23,  7,  6,  5, 63
        };
        value |= value >> 1;
        value |= value >> 2;
        value |= value >> 4;
        value |= value >> 8;
        value |= value >> 16;
        value |= value >> 32;
        return tab64[value * 0x03f6eaf2cd271461 >> 58];
    #else
        const uint8_t tab32[32] = {
             0,  9,  1, 10, 13, 21,  2, 29,
            11, 14, 16, 18, 22, 25,  3, 30,
             8, 12, 20, 28, 15, 17, 24,  7,
            19, 27, 23,  6, 26,  5,  4, 31
        };
        value |= value >> 1;
        value |= value >> 2;
        value |= value >> 4;
        value |= value >> 8;
        value |= value >> 16;
        return tab32[value * 0x07C4ACDDU >> 27];
    #endif
#endif
    }

    struct NoLock
    {
        void lock() {}
        void unlock() {}
    };


    /*************************************************************
     *                  Fixed Size Memory Pool                   *
     ************************************************************/

    // Very simple memory pool with fixed size blocks. allocate() and free() are O(1).
    // Memory overhead is 2 bits per block.
    //
    // For a memory pool with fixed size blocks you only need a stack. The stack is 
    // essentially a list of indexes of free blocks. 
    // - On allocation you pop the top index from it, and use it as your pointer. 
    // - On deallocation, you insert the index at the top.
    // 
    // To reduce memory use, I group blocks into chunks. Each chunk has a bitmap
    // which shows which blocks in it are free. We can quickly find a free block
    // in that bitmap using a bit scan. In this case the stack contains indexes of
    // chunks that are not full. 
    // - On allocation we take the top chunk from the stack, and remove it ONLY if 
    //   it becomes full.
    // - On deallocation we push the chunk into the stack ONLY if it was previously full.

    namespace fixedpool 
    {
        template<typename Index>
        FORCE_INLINE size_t get_chunk_size()
        {
            return sizeof(Index) * 8;
        }

        template<typename Index>
        FORCE_INLINE size_t get_number_chunks(size_t numberBlocks)
        {
            return (numberBlocks + get_chunk_size<Index>() - 1) / get_chunk_size<Index>();
        }

        template<typename Index>
        FORCE_INLINE size_t block_count_from_size(size_t _blockSize, size_t _bufferSize)
        {
            _bufferSize -= sizeof(Index) * 2;
            return size_t(_bufferSize / (_blockSize + 0.25));
        }
            
        template<
            typename LockType = NoLock,
            bool AllocFallback = false,  // If true and no blocks are available allocate using new[], otherwise throw std::bad_alloc.
            bool ExternalBuffer = false,  // If true the memory buffer is handled externally.
            typename Index = size_t  // Data type to use for indexes. Limits the maximum number of blocks, but also reduces the size of the pool object.
        >
        class FixedMemoryPool
        {
            uint8_t* buffer = nullptr;  // Buffer containing all blocks and metadata of the pool.        
            Index nonFullChunkCount = 0;
            Index allocatedBlocks = 0;
            Index numberBlocks = 0;
            Index blockSize = 1;
            [[no_unique_address]]
#ifdef _MSC_VER
            [[msvc::no_unique_address]]  // I hate this company
#endif
                LockType lock;

            void release_buffer()
            {
                if (!ExternalBuffer)
                    delete[] buffer;
                buffer = nullptr;
            }

            size_t get_chunk_size() const {
                return sizeof(Index) * 8;
            }

            size_t get_number_chunks()
            {
                return (numberBlocks + get_chunk_size() - 1) / get_chunk_size();
            }

            // Returns the pointers required for all the internal structures. The pointers are part of "buffer".
            void get_internal_pointers(Index** nonFullChunks, Index** chunkBitmaps, uint8_t** blocks)
            {
                size_t numberChunks = get_number_chunks();
                *nonFullChunks = (Index*)(buffer);
                *chunkBitmaps = (Index*)(buffer + numberChunks * sizeof(Index));
                *blocks = buffer + numberChunks * sizeof(Index) * 2;
            }

            size_t block_count_from_size(size_t _blockSize, size_t _bufferSize)
            {
                _bufferSize -= sizeof(Index) * 2;
                return size_t(_bufferSize / (_blockSize + 0.25));
            }

            void initialize_internal(uint8_t* _buffer, size_t _blockSize, size_t _numberBlocks, size_t _numberChunks)
            {
                release_buffer();
                numberBlocks = _numberBlocks;
                blockSize = _blockSize;
                allocatedBlocks = 0;
                nonFullChunkCount = _numberChunks;

                buffer = _buffer;
                Index* nonFullChunks;
                Index* chunkBitmaps;
                uint8_t* blocks;
                get_internal_pointers(&nonFullChunks, &chunkBitmaps, &blocks);

                for (size_t i = 0; i < _numberChunks; i++)
                    nonFullChunks[i] = i;
                for (size_t i = 0; i < _numberChunks; i++)
                    chunkBitmaps[i] = (Index)-1;
                // If we dont have a multiple of CHUNK_SIZE mark some blocks in last chunk as used
                if (_numberBlocks % get_chunk_size())
                    chunkBitmaps[_numberChunks - 1] >>= (get_chunk_size() - _numberBlocks % get_chunk_size());
            }

        public:

            FixedMemoryPool() {}

            FixedMemoryPool(size_t _blockSize, size_t _numberBlocks) requires (ExternalBuffer == false)
            {
                restart(_blockSize, _numberBlocks);
            }

            FixedMemoryPool(size_t _blockSize, uint8_t* _buffer, size_t _bufferSize) requires (ExternalBuffer == true)
            {
                restart(_blockSize, _buffer, _bufferSize);
            }

            ~FixedMemoryPool()
            {
                release_buffer();
            }

            void restart(size_t _blockSize, size_t _numberBlocks) requires (ExternalBuffer == false)
            {
                numberBlocks = _numberBlocks;
                size_t numberChunks = get_number_chunks();
                uint8_t* newBuffer = new uint8_t[_blockSize * _numberBlocks + numberChunks * sizeof(Index) * 2];
                initialize_internal(newBuffer, _blockSize, _numberBlocks, numberChunks);
            }

            void restart(size_t _blockSize, uint8_t* _buffer, size_t _bufferSize) requires (ExternalBuffer == true)
            {
                numberBlocks = block_count_from_size(_blockSize, _bufferSize);
                size_t numberChunks = get_number_chunks();
                initialize_internal(_buffer, _blockSize, numberBlocks, numberChunks);
            }

            FORCE_INLINE void* allocate()
            {
                lock.lock();

                if (nonFullChunkCount == 0) {
                    lock.unlock();
                    if (!AllocFallback)
                        throw std::bad_alloc();
                    return new uint8_t[blockSize];
                }

                Index* nonFullChunks;
                Index* chunkBitmaps;
                uint8_t* blocks;
                get_internal_pointers(&nonFullChunks, &chunkBitmaps, &blocks);

                Index chunkIndex = nonFullChunks[nonFullChunkCount - 1];
                Index bitmap = chunkBitmaps[chunkIndex];

                size_t freeBlock = unsafe_bit_scan_forward(bitmap);
                bitmap ^= (Index)1 << freeBlock;

                chunkBitmaps[chunkIndex] = bitmap;
                nonFullChunkCount -= (bitmap == 0);
                allocatedBlocks += 1;

                lock.unlock();

                uint8_t* ptr = blocks + blockSize * (chunkIndex * get_chunk_size() + freeBlock);
                return ptr;
            }

            FORCE_INLINE void free(void* ptr)
            {
                if (ptr == nullptr)
                    return;

                Index* nonFullChunks;
                Index* chunkBitmaps;
                uint8_t* blocks;
                get_internal_pointers(&nonFullChunks, &chunkBitmaps, &blocks);

                size_t index = size_t((uint8_t*)ptr - blocks) / blockSize;
                // Pointer came from new allocation
                if (index >= numberBlocks) {
                    delete[](uint8_t*)ptr;
                    return;
                }

                lock.lock();

                size_t chunkIndex = index / get_chunk_size();
                size_t blockIndex = index % get_chunk_size();

                //This chunk now has one free block; append to stack
                if (chunkBitmaps[chunkIndex] == 0) {
                    nonFullChunks[nonFullChunkCount] = chunkIndex;
                    nonFullChunkCount += 1;
                }
                chunkBitmaps[chunkIndex] |= (Index)1 << blockIndex;
                allocatedBlocks -= 1;

                lock.unlock();
            }

            size_t get_allocated_blocks()
            {
                lock.lock();
                size_t result = allocatedBlocks;
                lock.unlock();
                return result;
            }

            bool empty()
            {
                return get_allocated_blocks() == 0;
            }

            bool full()
            {
                return get_allocated_blocks() == numberBlocks;
            }

            size_t get_block_size()
            {
                return blockSize;
            }

            size_t get_number_blocks()
            {
                return numberBlocks;
            }

            // Returns the ammount of memory used by this pool, that is, the sum of the data blocks and required metadata
            size_t get_internal_allocated_memory()
            {
                return numberBlocks * blockSize + get_number_chunks() * sizeof(Index) * 2;
            }

            // Returns the buffer that the pool uses internally for blocks and metadata
            uint8_t* get_internal_buffer()
            {
                return buffer;
            }
        };
    }


    /*************************************************************
     *                 Variable Size Memory Pool                 *
     ************************************************************/

    // A memory pool which can allocate and deallocate any number of bytes with 
    // relatively low overhead. allocate() and free() are O(log n), with n the 
    // number of allocated segments in the pool.
    //
    // The pool is composed of blocks of 64 bytes. It has 2 different 
    // allocation mechanisms depending on the size:
    // - >=8192 bytes: uses a binary tree to quickly find a free segment with 
    //   enough capacity for the given allocation. A segment can be composed 
    //   of any number of blocks. Each block has associated information, such 
    //   as whether is free or occupied, the length of the segment it belongs 
    //   to and an index pointing to the previous segment.
    // - <8192 bytes: takes several contiguous blocks using the previous algorithm,
    //   and initializes a FixedMemoryPool in them with an appropiate block size.
    //   It uses a linked list to keep track of pools with available blocks for 
    //   each size. 

    namespace variablepool 
    {
        // Used by the binary tree in VariableMemoryPool
        template<typename T>
        class MyPoolAlloc {
        public:

            fixedpool::FixedMemoryPool<>* leafPool;
            fixedpool::FixedMemoryPool<>* internalPool;

            typedef size_t     size_type;
            typedef ptrdiff_t  difference_type;
            typedef T* pointer;
            typedef const T* const_pointer;
            typedef T& reference;
            typedef const T& const_reference;
            typedef T          value_type;
            typedef std::true_type propagate_on_container_move_assignment;
            typedef std::true_type is_always_equal;

            template<typename X>
            using rebind = MyPoolAlloc<X>;

            MyPoolAlloc(fixedpool::FixedMemoryPool<>* _leafPool, fixedpool::FixedMemoryPool<>* _internalPool) noexcept {
                leafPool = _leafPool;
                internalPool = _internalPool;
            }

            MyPoolAlloc() noexcept {
                leafPool = nullptr;
                internalPool = nullptr;
            }

            MyPoolAlloc(const MyPoolAlloc& alloc) noexcept {
                leafPool = alloc.leafPool;
                internalPool = alloc.internalPool;
            }

            template<typename X>
            MyPoolAlloc(const MyPoolAlloc<X>& alloc) noexcept {
                leafPool = alloc.leafPool;
                internalPool = alloc.internalPool;
            }

            ~MyPoolAlloc() noexcept {};

            pointer address(reference __x) const { return &__x; }

            const_pointer address(const_reference __x) const { return &__x; }

            pointer allocate(size_type __n, const void* hint = 0) {
                if (__n * sizeof(T) <= leafPool->get_block_size())
                    return reinterpret_cast<T*>(leafPool->allocate());
                return reinterpret_cast<T*>(internalPool->allocate());
            }

            void deallocate(pointer __p, size_type __n) {
                if (__n * sizeof(T) <= leafPool->get_block_size())
                    leafPool->free(reinterpret_cast<uint8_t*>(__p));
                else
                    internalPool->free(reinterpret_cast<uint8_t*>(__p));
            }

            size_type max_size() const {
                return SIZE_MAX;
            }

            void construct(pointer __p, const T& __val) {
                ::new(__p) T(__val);
            }

            void destroy(pointer __p) {
                __p->~T();
            }
        };

        // Pools for suballocations will be small; we can use uint16_t indexes to further reduce their overhead
        using SuballocationPool = fixedpool::FixedMemoryPool<NoLock, false, true, uint16_t>;

        struct VarBlockNode;

        struct SuballocationNode
        {
            SuballocationPool pool;
            VarBlockNode* next;
            VarBlockNode* prev;
            uint8_t allocID;
        };

        struct VarBlockNode 
        {
            size_t length;
            size_t previous;
            SuballocationNode suballocation;
            bool isSuballocation;
            bool isFree;
    #if IS_64BIT
            uint8_t padding[6];
    #else
            uint8_t padding[30];
    #endif
        };

        struct FreeSegment {
            size_t length;
            size_t index;

            FreeSegment() {}
            FreeSegment(size_t _len, size_t _idx) {
                length = _len;
                index = _idx;
            }
        };

        // For memory efficiency the bTree will only store the block index, and
        // we will fetch the length from the blockNodes array.
        struct bTreeFreeSegment {
            size_t index;

            bTreeFreeSegment() {}
            bTreeFreeSegment(size_t index) {
                this->index = index;
            }
            bTreeFreeSegment(const FreeSegment& chunk) {
                this->index = chunk.index;
            }
        };

        const int VAR_ALLOC_ID = 255;  // Use variable block allocation
        const size_t VAR_ALLOC_THRESHOLD = 8192;  // Use variable block allocation for sizes larger than this
        const size_t VAR_ALLOC_GRANULARITY = sizeof(VarBlockNode);
        const int NEXT_LINK = 1;
        const int PREV_LINK = 0;

        // Segment length in blocks for each allocation ID
        const int ALLOCID_TO_SEGMENT[] = {
            111, 101,  99, 100,  99,  95,  95,  96,  93,  92,  92,  95,  92,  99,  93,  92,  // 1 - 16 bytes
             99, 102, 100,  95, 101,  95,  92,  91,  92,  94,  98,  91,  97,  97, 107, 102,  // 17 - 32 bytes
             97,  97, 103,  90, 109,  99, 102, 105,  99,  94,  95,  95,  92,  98, 109, 127,  // 2^5 - 2^6 bytes
             95, 104,  93, 103, 112,  98, 101, 104, 102, 106, 105, 109, 100, 111, 103, 145,  // 2^6 - 2^7 bytes
             98,  97, 100, 113, 100, 102,  98, 154, 116, 101, 105, 116, 109, 109, 101, 157,  // 2^7 - 2^8 bytes
             98, 122, 100, 161, 100, 127, 121, 163, 119, 124, 115, 162, 109, 128, 101, 169,  // 2^8 - 2^9 bytes
            128, 163, 124, 171, 137, 166, 127, 169, 138, 170, 149, 169, 131, 166, 140, 177,  // 2^9 - 2^10 bytes
            171, 181, 172, 181, 169, 177, 185, 169, 176, 183, 190, 169, 175, 181, 187, 193,  // 2^10 - 2^11 bytes
            171, 181, 191, 201, 169, 177, 185, 193, 201, 209, 217, 169, 175, 181, 187, 193,  // 2^11 - 2^12 bytes
            205, 217, 229, 241, 169, 177, 185, 193, 201, 209, 217, 225, 233, 241, 249, 257,  // 2^12 - 2^13 bytes
        };

        // Allocation size for each allocation ID
        const int ALLODID_TO_SIZE[] = {
               1,    2,    3,    4,    5,    6,    7,    8,    9,   10,   11,   12,   13,   14,   15,   16,  // 1 - 16 bytes
              17,   18,   19,   20,   21,   22,   23,   24,   25,   26,   27,   28,   29,   30,   31,   32,  // 17 - 32 bytes
              34,   36,   38,   40,   42,   44,   46,   48,   50,   52,   54,   56,   58,   60,   62,   64,  // 2^5 - 2^6 bytes
              68,   72,   76,   80,   84,   88,   92,   96,  100,  104,  108,  112,  116,  120,  124,  128,  // 2^6 - 2^7 bytes
             136,  144,  152,  160,  168,  176,  184,  192,  200,  208,  216,  224,  232,  240,  248,  256,  // 2^7 - 2^8 bytes
             272,  288,  304,  320,  336,  352,  368,  384,  400,  416,  432,  448,  464,  480,  496,  512,  // 2^8 - 2^9 bytes
             544,  576,  608,  640,  672,  704,  736,  768,  800,  832,  864,  896,  928,  960,  992, 1024,  // 2^9 - 2^10 bytes
            1088, 1152, 1216, 1280, 1344, 1408, 1472, 1536, 1600, 1664, 1728, 1792, 1856, 1920, 1984, 2048,  // 2^10 - 2^11 bytes
            2176, 2304, 2432, 2560, 2688, 2816, 2944, 3072, 3200, 3328, 3456, 3584, 3712, 3840, 3968, 4096,  // 2^11 - 2^12 bytes
            4352, 4608, 4864, 5120, 5376, 5632, 5888, 6144, 6400, 6656, 6912, 7168, 7424, 7680, 7936, 8192,  // 2^12 - 2^13 bytes
        };
        
        template<typename LockType = NoLock>
        class VariableMemoryPool
        {
            size_t numberBlocks = 0;
            VarBlockNode* suballocationHeads[160];
            union {
                uint8_t* data = nullptr;
                VarBlockNode* blocks;
            };
            size_t* beginOfSegment = nullptr;  // Bitmap, showing which blocks are the start of a segment

            struct FreeSegmentComparator {
                const VariableMemoryPool& pool;
                explicit FreeSegmentComparator(const VariableMemoryPool& p) : pool(p) {}
                using is_transparent = void;

                bool operator()(const bTreeFreeSegment& lhs, const bTreeFreeSegment& rhs) const {
                    if (pool.blocks[lhs.index].length == pool.blocks[rhs.index].length)
                        return lhs.index < rhs.index;
                    return pool.blocks[lhs.index].length < pool.blocks[rhs.index].length;
                }

                // To erase or find a free segment if we already know its length and index 
                bool operator()(const bTreeFreeSegment& lhs, const FreeSegment& rhs) const {
                    if (pool.blocks[lhs.index].length == rhs.length)
                        return lhs.index < rhs.index;
                    return pool.blocks[lhs.index].length < rhs.length;
                }
                bool operator()(const FreeSegment& lhs, const bTreeFreeSegment& rhs) const {
                    if (lhs.length == pool.blocks[rhs.index].length)
                        return lhs.index < rhs.index;
                    return lhs.length < pool.blocks[rhs.index].length;
                }

                // To find the smallest free chunk that is at least of the given length
                bool operator()(const bTreeFreeSegment& lhs, size_t len) const {
                    return pool.blocks[lhs.index].length < len;
                }
                bool operator()(size_t len, const bTreeFreeSegment& rhs) const {
                    return len < pool.blocks[rhs.index].length;
                }
            };

            fixedpool::FixedMemoryPool<> bTreeLeafPool;
            fixedpool::FixedMemoryPool<> bTreeInternalPool;
            MyPoolAlloc<bTreeFreeSegment> bTreeAlloc;
            phmap::btree_set<bTreeFreeSegment, FreeSegmentComparator, MyPoolAlloc<bTreeFreeSegment>> freeSegments;
            bool allocFallback = false;
            LockType lock;

            size_t size_to_alloc_id(size_t size)
            {
                size -= 1;
                if (size < 32)
                    return size;
                int id = unsafe_int_log2(size);
                id = (id - 3) * 16 + (size >> (id - 4) & 15);
                return id;
            }

            void flip_begin_of_segment(size_t blockIndex) 
            {
                beginOfSegment[blockIndex / (sizeof(size_t) * 8)] ^= (size_t)1 << blockIndex % (sizeof(size_t) * 8);
            }

            // The pointer given to free() might not point to the beginning of the segment. We have to search for it.
            size_t scan_begin_of_segment(size_t blockIndex) 
            {
                blockIndex -= 1;
                size_t bufferIndex = blockIndex / (sizeof(size_t) * 8);
                size_t bitIndex = blockIndex % (sizeof(size_t) * 8);
                size_t bitmap = beginOfSegment[bufferIndex] & SIZE_MAX >> (bitIndex ^ 63);
                if (bitmap == 0) {
                    blockIndex -= bitIndex;
                    bufferIndex -= 1;
                    while (true) {
                        if (beginOfSegment[bufferIndex] == 0) {
                            blockIndex -= 64;
                            bufferIndex -= 1;
                            continue;
                        }
                        return blockIndex - (sizeof(size_t) * 8) + unsafe_int_log2(beginOfSegment[bufferIndex]);
                    }
                }
                return blockIndex - bitIndex + unsafe_int_log2(bitmap);
            }

            void* allocate_varblock(size_t blocksRequested)
            {
                auto freeSegment = freeSegments.lower_bound(blocksRequested);
                if (freeSegment == freeSegments.end())
                    return nullptr;

                size_t blockIndex = freeSegment->index;
                size_t segmentLength = blocks[blockIndex].length;
                freeSegments.erase(freeSegment);

                blocks[blockIndex].isFree = false;
                blocks[blockIndex].isSuballocation = false;
                blocks[blockIndex].length = blocksRequested;
                flip_begin_of_segment(blockIndex);

                if (segmentLength > blocksRequested) {
                    blocks[blockIndex + blocksRequested + 1].isFree = true;
                    blocks[blockIndex + blocksRequested + 1].length = segmentLength - blocksRequested - 1;
                    blocks[blockIndex + blocksRequested + 1].previous = blockIndex;
                    freeSegments.insert(FreeSegment(segmentLength - blocksRequested - 1, blockIndex + blocksRequested + 1));
                }

                //Update the previous link of the next chunk
                if (blockIndex + segmentLength + 1 != numberBlocks) {
                    blocks[blockIndex + segmentLength + 1].previous =
                        blockIndex + (segmentLength > blocksRequested ? blocksRequested + 1 : 0);
                }
                return data + (blockIndex + 1) * VAR_ALLOC_GRANULARITY;
            }

            void deallocate_varblock(size_t blockIndex)
            {
                size_t segmentLength = blocks[blockIndex].length;
                flip_begin_of_segment(blockIndex);

                // Merge with next segment
                size_t nextSegment = blockIndex + segmentLength + 1;
                if (nextSegment != numberBlocks) {
                    if (blocks[nextSegment].isFree) {
                        freeSegments.erase(FreeSegment(blocks[nextSegment].length, nextSegment));
                        segmentLength += blocks[nextSegment].length + 1;
                    }
                }
                // Merge with previous segment
                if (blockIndex != 0) {
                    size_t previousSegment = blocks[blockIndex].previous;
                    if (blocks[previousSegment].isFree) {
                        freeSegments.erase(FreeSegment(blocks[previousSegment].length, previousSegment));
                        segmentLength += blocks[previousSegment].length + 1;
                        blockIndex = previousSegment;
                    }
                }

                // Update the previous link of the next segment
                if (blockIndex + segmentLength + 1 != numberBlocks) 
                    blocks[blockIndex + segmentLength + 1].previous = blockIndex;

                blocks[blockIndex].isFree = true;
                blocks[blockIndex].length = segmentLength;
                freeSegments.insert({ blockIndex });
            }

            void deallocate_fixblock(void* allocPtr, size_t blockIndex)
            {
                VarBlockNode* node = blocks + blockIndex;
                size_t allocID = node->suballocation.allocID;
                bool isFull = node->suballocation.pool.full();
                node->suballocation.pool.free(allocPtr);

                // If the pool is now empty free its segment so that it can be used by others
                if (node->suballocation.pool.empty()) {
                    deallocate_varblock(blockIndex);

                    // And remove from linked list...
                    if (suballocationHeads[allocID] == node)
                        suballocationHeads[allocID] = node->suballocation.next;
                    if (node->suballocation.next != nullptr) 
                        node->suballocation.next->suballocation.prev = node->suballocation.prev;
                    if (node->suballocation.prev != nullptr) 
                        node->suballocation.prev->suballocation.next = node->suballocation.next;
                }
                // If the pool was full, now it isn't. Put it in the list of available fixed pools.
                else if (isFull) {
                    node->suballocation.prev = nullptr;
                    node->suballocation.next = suballocationHeads[allocID];
                    if (suballocationHeads[allocID] != nullptr) 
                        suballocationHeads[allocID]->suballocation.prev = node;
                    suballocationHeads[allocID] = node;
                }
            }

            void* allocate_fixblock(size_t allocID)
            {
                if (suballocationHeads[allocID] != nullptr)
                {
                    VarBlockNode* node = suballocationHeads[allocID];
                    void* ptr = node->suballocation.pool.allocate();
                    // Remove the pool from the free pool linked list
                    if (node->suballocation.pool.full()) {
                        suballocationHeads[allocID] = node->suballocation.next;
                        if (node->suballocation.next != nullptr) 
                            node->suballocation.next->suballocation.prev = nullptr;
                    }
                    return ptr;
                }

                void* ptr = allocate_varblock(ALLOCID_TO_SEGMENT[allocID]);
                if (!ptr)
                    return nullptr;
                VarBlockNode* node = (VarBlockNode*)ptr - 1;

                node->isSuballocation = true;
                node->suballocation.allocID = allocID;
                node->suballocation.pool.restart(
                    ALLODID_TO_SIZE[allocID], (uint8_t*)ptr, VAR_ALLOC_GRANULARITY * ALLOCID_TO_SEGMENT[allocID]
                );
                // Add the pool to the free pool linked list
                suballocationHeads[allocID] = node;
                node->suballocation.next = nullptr;
                node->suballocation.prev = nullptr;

                return node->suballocation.pool.allocate();
            }

        public:

            VariableMemoryPool()
                : bTreeAlloc(&bTreeLeafPool, &bTreeInternalPool), freeSegments(FreeSegmentComparator(*this), bTreeAlloc) {}
            VariableMemoryPool(size_t _numberBlocks, bool _allocFallback = false)
                : bTreeAlloc(&bTreeLeafPool, &bTreeInternalPool), freeSegments(FreeSegmentComparator(*this), bTreeAlloc) {
                restart(_numberBlocks, _allocFallback);
            }
            ~VariableMemoryPool() {
                delete[] data;
                delete[] beginOfSegment;
            }

            void restart(size_t _poolSize, bool _allocFallback = false)
            {
                delete[] data;
                data = nullptr;
                delete[] beginOfSegment;
                beginOfSegment = nullptr;

                // Round pool size to next granularity multiple
                _poolSize = _poolSize + (16 - _poolSize % VAR_ALLOC_GRANULARITY);
                numberBlocks = _poolSize / VAR_ALLOC_GRANULARITY;
                data = new uint8_t[_poolSize];
                size_t numberChunks = fixedpool::get_number_chunks<size_t>(numberBlocks);
                beginOfSegment = new size_t[numberChunks];

                // Worst case we can have repeating minimum size VAR_ALLOC_THRESHOLD segments followed by a
                // tiny free segment of size VAR_ALLOC_GRANULARITY. 
                size_t maxFreeSegments = _poolSize / (VAR_ALLOC_THRESHOLD / 2) + 1;

                // A leaf node of the binary tree can hold up to 512 bytes of data. This includes space 
                // for 2 qwords or dwords for 64 or 32 bit architectures. So the number of elements per 
                // leaf node is given by kNodeValues = (512 - sizeof(void*) * 2) / sizeof(ValueType), 
                // with a minimum value of 3. The size it occupies is kNodeValues * sizeof(ValueType) + sizeof(void*) * 2
                int kNodeValues = (512 - sizeof(void*) * 2) / sizeof(bTreeFreeSegment);
                if (kNodeValues < 3)
                    kNodeValues = 3;
                int kLeafNodeSize = kNodeValues * sizeof(bTreeFreeSegment) + sizeof(void*) * 2;
                // A non leaf node can also hold kNodeValues, but instead of 2 qwords/dwords it holds
                // kNodeValues + 3 of them. Each of them can have up to kNodeValues + 1 children.
                int kInternalNodeSize = kNodeValues * sizeof(bTreeFreeSegment) + sizeof(void*) * (kNodeValues + 3);

                // These are approximations, real count is usually much lower 
                // but we want to avoid running out of nodes in the memory pools.
                size_t kMaxLeafCount = maxFreeSegments / ((kNodeValues + 1) / 2) + 1;
                size_t kMaxInternalCount = kMaxLeafCount / ((kNodeValues + 1) / 2) + 1;

                freeSegments.clear();
                bTreeLeafPool.restart(kLeafNodeSize, kMaxLeafCount);
                bTreeInternalPool.restart(kInternalNodeSize, kMaxInternalCount);

                blocks[0].isFree = true;
                blocks[0].length = numberBlocks - 1;
                blocks[0].previous = 0;
                freeSegments.insert(bTreeFreeSegment(0));
                for (size_t i = 0; i < 160; i++)
                    suballocationHeads[i] = nullptr;
                memset(beginOfSegment, 0, sizeof(size_t) * numberChunks);
                allocFallback = _allocFallback;
            }

            void* allocate(size_t size)
            {
                if (size == 0)
                    return nullptr;
                size_t blocksRequested = (size + VAR_ALLOC_GRANULARITY - 1) / VAR_ALLOC_GRANULARITY;

                lock.lock();

                void* ptr = nullptr;
                if (size < VAR_ALLOC_THRESHOLD) {
                    int id = size_to_alloc_id(size);
                    if (id != VAR_ALLOC_ID) 
                        ptr = allocate_fixblock(id);
                }

                if (!ptr) 
                    ptr = allocate_varblock(blocksRequested);

                lock.unlock();

                if (ptr)
                    return ptr;
                if (!allocFallback)
                    throw std::bad_alloc();
                return new uint8_t[size];
            }

            void free(void* ptr)
            {
                if (ptr == nullptr)
                    return;

                size_t blockIndex = size_t((uint8_t*)ptr - data) / VAR_ALLOC_GRANULARITY;
                // Pointer came from new allocation
                if (blockIndex >= numberBlocks) {
                    delete[] (uint8_t*)ptr;
                    return;
                }

                lock.lock();

                // Search for the segment header
                blockIndex = scan_begin_of_segment(blockIndex);
                // Check if this pointer is part of a suballocation inside a 
                // fixed memory pool or if it comes from a variable block allocation.
                if (blocks[blockIndex].isSuballocation)
                    deallocate_fixblock(ptr, blockIndex);
                else 
                    deallocate_varblock(blockIndex);

                lock.unlock();
            }

            // Returns the ammount of memory used by this pool, that is, the sum of the data blocks and required metadata
            size_t get_internal_allocated_memory()
            {
                size_t usedMemory = VAR_ALLOC_GRANULARITY * numberBlocks;  // data
                usedMemory += fixedpool::get_number_chunks<size_t>(numberBlocks) * sizeof(size_t);  // beginOfSegment
                usedMemory += bTreeLeafPool.get_internal_allocated_memory();
                usedMemory += bTreeInternalPool.get_internal_allocated_memory();
                return usedMemory;
            }
        };
    }
}
