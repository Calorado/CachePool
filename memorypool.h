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

namespace mempool {

    // Undefined behaviour when value == 0
    size_t unsafe_bit_scan_forward(const size_t value) {
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

    size_t unsafe_int_log2(size_t value) {
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
        
        void* allocate()
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

        void free(void* ptr)
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
                delete[] (uint8_t*)ptr;
                return;
            }

            lock.lock();

            size_t chunkIndex = index / get_chunk_size();
            size_t blockIndex = index % get_chunk_size();

            Index bitmap = chunkBitmaps[chunkIndex];
            //This chunk now has one free block; append to stack
            if (bitmap == 0) {
                nonFullChunks[nonFullChunkCount] = chunkIndex;
                nonFullChunkCount += (bitmap == 0);
            }
            bitmap |= (Index)1 << blockIndex;
            chunkBitmaps[chunkIndex] = bitmap;
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


    /*************************************************************
     *                 Variable Size Memory Pool                 *
     ************************************************************/

    // A memory pool which can allocate and deallocate any number of bytes with 
    // relatively low overhead. allocate() and free() are O(log n), with n the 
    // number of allocated segments in the pool.
    //
    // The pool is composed of blocks of a fixed 1024 bytes. It has 2 different 
    // allocation mechanisms depending on the size:
    // - >=3072 bytes: uses a binary tree to quickly find a free segment with 
    //   enough capacity for the given allocation. Each block has associated
    //   information, such as whether is free or occupied, the length of the 
    //   segment it belongs to and an index pointing to the previous segment.
    // - <3072 bytes: takes 32 contiguous blocks using the previous algorithm,
    //   and initializes a FixedMemoryPool in them with an appropiate block size.
    //   It uses a linked list to keep track of pools with available blocks for 
    //   each size. 

    // Used by the binary tree in VariableMemoryPool
    template<typename T>
    class MyPoolAlloc {
    public:

        FixedMemoryPool<>* leafPool;
        FixedMemoryPool<>* internalPool;

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

        MyPoolAlloc(FixedMemoryPool<>* _leafPool, FixedMemoryPool<>* _internalPool) noexcept {
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

    struct VarBlockNode 
    {
        size_t isFree : 1;
        size_t firstOfSegment : 1;  // Is this block the first of a given allocation?
        size_t allocID : 8;  // Suballocation size used by this block, or VAR_ALLOC_ID if it doesn't use suballocation
#if IS_64BIT
        size_t length : 54;
#else
        size_t length : 22;
#endif
        size_t previous;  // Index of previous segment
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

    // Pools for suballocations will be small; we can uint16_t indexes to further reduce their overhead
    using SuballocationPool = FixedMemoryPool<NoLock, false, true, uint16_t>;

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
    const size_t VAR_POOL_BLOCK_SIZE = 1024;
    const size_t VAR_ALLOC_THRESHOLD = 8192;  // Always use variable block allocation for sizes larger than this
    const int NEXT_LINK = 1;
    const int PREV_LINK = 0;

    const int VAR_POOL_SUBALLOC_SEGMENT_SIZE[] = {
        16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,  // 1 - 16 bytes
        16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,  // 2^4 - 2^6 bytes
        25, 22, 18, 22, 17, 17, 25, 24, 21, 26, 26, 25, 23, 25, 27, 24,  // 2^6 - 2^8 bytes
        22, 26, 20, 26, 22, 25, 31, 28, 22, 27, 20, 25, 22, 29, 31,  0,  // 2^8 - 2^10 bytes
        35, 39, 40, 47, 44, 51, 47,  0, 70, 68, 69,  0, 75, 81, 79,  0,  // 2^10 - 2^12 bytes
        95,  0, 94,  0, 98,  0, 98,  0,                                  // 2^12 - 2^13 bytes
    };
    
    template<typename LockType = NoLock>
    class VariableMemoryPool
    {
        size_t numberBlocks = 0;
        uint8_t* suballocationHeads[88];
        uint8_t* data = nullptr;
        VarBlockNode* blockNodes = nullptr;

        struct FreeSegmentComparator {
            const VariableMemoryPool& pool;
            explicit FreeSegmentComparator(const VariableMemoryPool& p) : pool(p) {}
            using is_transparent = void;

            bool operator()(const bTreeFreeSegment& lhs, const bTreeFreeSegment& rhs) const {
                if (pool.blockNodes[lhs.index].length == pool.blockNodes[rhs.index].length)
                    return lhs.index < rhs.index;
                return pool.blockNodes[lhs.index].length < pool.blockNodes[rhs.index].length;
            }

            // To erase or find a free segment if we already know its length and index 
            bool operator()(const bTreeFreeSegment& lhs, const FreeSegment& rhs) const {
                if (pool.blockNodes[lhs.index].length == rhs.length)
                    return lhs.index < rhs.index;
                return pool.blockNodes[lhs.index].length < rhs.length;
            }
            bool operator()(const FreeSegment& lhs, const bTreeFreeSegment& rhs) const {
                if (lhs.length == pool.blockNodes[rhs.index].length)
                    return lhs.index < rhs.index;
                return lhs.length < pool.blockNodes[rhs.index].length;
            }

            // To find the smallest free chunk that is at least of the given length
            bool operator()(const bTreeFreeSegment& lhs, size_t len) const {
                return pool.blockNodes[lhs.index].length < len;
            }
            bool operator()(size_t len, const bTreeFreeSegment& rhs) const {
                return len < pool.blockNodes[rhs.index].length;
            }
        };

        FixedMemoryPool<> bTreeLeafPool;
        FixedMemoryPool<> bTreeInternalPool;
        MyPoolAlloc<bTreeFreeSegment> bTreeAlloc;
        phmap::btree_set<bTreeFreeSegment, FreeSegmentComparator, MyPoolAlloc<bTreeFreeSegment>> freeSegments;
        bool allocFallback = false;
        LockType lock;

        size_t alloc_id_to_size(size_t id)
        {
            id += 1;
            if (id <= 16)
                return id;
            int bits = id / 8 - 1;
            size_t size = (8 | (id & 7)) << bits;
            return size;
        }

        size_t size_to_alloc_id(size_t size)
        {
            size -= 1;
            if (size < 16)
                return size;
            int id = unsafe_int_log2(size);
            id = (id - 2) * 8 + (size >> (id - 3) & 7);
            // Fits perfectly in one or more blocks. Using suballocation would only add overhead.
            if (alloc_id_to_size(id) % VAR_POOL_BLOCK_SIZE == 0)
                return VAR_ALLOC_ID;
            return id;
        }

        void* allocate_varblock(size_t blocksRequested)
        {
            auto freeSegment = freeSegments.lower_bound(blocksRequested);
            if (freeSegment == freeSegments.end())
                return nullptr;

            size_t blockIndex = freeSegment->index;
            size_t chunkLength = blockNodes[blockIndex].length;
            freeSegments.erase(freeSegment);

            blockNodes[blockIndex].isFree = false;
            blockNodes[blockIndex].length = blocksRequested;

            if (chunkLength > blocksRequested) {
                blockNodes[blockIndex + blocksRequested].isFree = true;
                blockNodes[blockIndex + blocksRequested].length = chunkLength - blocksRequested;
                blockNodes[blockIndex + blocksRequested].previous = blockIndex;
                freeSegments.insert(FreeSegment(chunkLength - blocksRequested, blockIndex + blocksRequested));
            }
            //Update the previous link of the next chunk
            if (blockIndex + chunkLength != numberBlocks) {
                blockNodes[blockIndex + chunkLength].previous =
                    blockIndex + (chunkLength > blocksRequested ? blocksRequested : 0);
            }
            return data + blockIndex * VAR_POOL_BLOCK_SIZE;
        }

        void deallocate_varblock(size_t blockIndex)
        {
            size_t segmentLength = blockNodes[blockIndex].length;

            // Merge with next chunk
            if (blockIndex + segmentLength != numberBlocks) {
                if (blockNodes[blockIndex + segmentLength].isFree) {
                    freeSegments.erase(FreeSegment(blockNodes[blockIndex + segmentLength].length, blockIndex + segmentLength));
                    segmentLength += blockNodes[blockIndex + segmentLength].length;
                }
            }
            // Merge with previous chunk
            if (blockIndex != 0) {
                size_t previousSegment = blockNodes[blockIndex].previous;
                if (blockNodes[previousSegment].isFree) {
                    freeSegments.erase(FreeSegment(blockNodes[previousSegment].length, previousSegment));
                    segmentLength += blockNodes[previousSegment].length;
                    blockIndex = previousSegment;
                }
            }

            // Update the previous link of the next chunk
            if (blockIndex + segmentLength != numberBlocks)
                blockNodes[blockIndex + segmentLength].previous = blockIndex;

            blockNodes[blockIndex].isFree = true;
            blockNodes[blockIndex].length = segmentLength;
            freeSegments.insert({ blockIndex });
        }

        void deallocate_fixblock(void* allocPtr, size_t blockIndex)
        {
            while (!blockNodes[blockIndex].firstOfSegment)
                blockIndex -= 1;

            uint8_t* blockPtr = data + blockIndex * VAR_POOL_BLOCK_SIZE;
            SuballocationPool* pool = (SuballocationPool*)blockPtr;
            uint8_t** links = (uint8_t**)(blockPtr + sizeof(SuballocationPool));
            size_t allocID = blockNodes[blockIndex].allocID;

            bool isFull = pool->full();
            pool->free(allocPtr);

            // If the pool is now empty free its segment so that it can be used by others
            if (pool->empty()) {
                deallocate_varblock(blockIndex);

                // And remove from linked list...
                if (suballocationHeads[allocID] == blockPtr)
                    suballocationHeads[allocID] = links[NEXT_LINK];
                if (links[NEXT_LINK] != nullptr) {
                    uint8_t** nextLinks =  (uint8_t**)(links[NEXT_LINK] + sizeof(SuballocationPool));
                    nextLinks[PREV_LINK] = links[PREV_LINK];
                }
                if (links[PREV_LINK] != nullptr) {
                    uint8_t** prevLinks =  (uint8_t**)(links[PREV_LINK] + sizeof(SuballocationPool));
                    prevLinks[NEXT_LINK] = links[NEXT_LINK];
                }
            }
            // If the pool was full, now it isn't. Put it in the list of available fixed pools.
            else if (isFull) {
                links[PREV_LINK] = nullptr;
                links[NEXT_LINK] = suballocationHeads[allocID];
                if (suballocationHeads[allocID] != nullptr) {
                    uint8_t** headLinks =  (uint8_t**)(suballocationHeads[allocID] + sizeof(SuballocationPool));
                    headLinks[PREV_LINK] = blockPtr;
                }
                suballocationHeads[allocID] = blockPtr;
            }
        }

        void* allocate_fixblock(size_t allocID)
        {
            if (suballocationHeads[allocID] != nullptr)
            {
                SuballocationPool* pool = (SuballocationPool*)suballocationHeads[allocID];
                void* ptr = pool->allocate();

                // Remove the pool from the free pool linked list
                if (pool->full()) {
                    uint8_t** links = (uint8_t**)(suballocationHeads[allocID] + sizeof(SuballocationPool));
                    suballocationHeads[allocID] = links[NEXT_LINK];
                    if (links[NEXT_LINK] != nullptr) {
                        links = (uint8_t**)(links[NEXT_LINK] + sizeof(SuballocationPool));
                        links[PREV_LINK] = nullptr;
                    }
                }
                return ptr;
            }

            void* ptr = allocate_varblock(VAR_POOL_SUBALLOC_SEGMENT_SIZE[allocID]);
            if (!ptr)
                return nullptr;
            size_t blockIndex = size_t((uint8_t*)ptr - data) / VAR_POOL_BLOCK_SIZE;

            size_t chunkOverhead = sizeof(SuballocationPool) + sizeof(uint8_t*) * 2;
            SuballocationPool* pool = new (ptr) SuballocationPool(
                alloc_id_to_size(allocID), (uint8_t*)ptr + chunkOverhead, 
                VAR_POOL_BLOCK_SIZE * VAR_POOL_SUBALLOC_SEGMENT_SIZE[allocID] - chunkOverhead
            );

            // Add pool to free pool linked list
            suballocationHeads[allocID] = (uint8_t*)ptr;
            uint8_t** links = (uint8_t**)(suballocationHeads[allocID] + sizeof(SuballocationPool));
            links[PREV_LINK] = nullptr;
            links[NEXT_LINK] = nullptr;

            for (size_t i = 0; i < VAR_POOL_SUBALLOC_SEGMENT_SIZE[allocID]; i++) {
                blockNodes[blockIndex + i].firstOfSegment = i == 0;
                blockNodes[blockIndex + i].allocID = allocID;
            }

            ptr = pool->allocate();
            return ptr;
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
            delete[] blockNodes;
        }

        void restart(size_t _poolSize, bool _allocFallback = false)
        {
            delete[] data;
            data = nullptr;
            delete[] blockNodes;
            blockNodes = nullptr;

            numberBlocks = (_poolSize + VAR_POOL_BLOCK_SIZE - 1) / VAR_POOL_BLOCK_SIZE;
            data = new uint8_t[VAR_POOL_BLOCK_SIZE * numberBlocks];
            blockNodes = new VarBlockNode[numberBlocks];

            // Maximum ammount of free segments we can have is ~(numberBlocks / 2), with alternating 
            // used and free blocks, otherwise they would get merged.
            size_t maxFreeSegments = (numberBlocks + 1) / 2;

            // A leaf node of the binary tree can hold up to 1024 bytes of data. This includes space 
            // for 2 qwords or dwords for 64 or 32 bit architectures. So the number of elements per 
            // leaf node is given by kNodeValues = (1024 - sizeof(void*) * 2) / sizeof(ValueType), 
            // with a minimum value of 3. The size it occupies is kNodeValues * sizeof(ValueType) + sizeof(void*) * 2
            int kNodeValues = (1024 - sizeof(void*) * 2) / sizeof(bTreeFreeSegment);
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

            allocFallback = _allocFallback;
            blockNodes[0].isFree = true;
            blockNodes[0].length = numberBlocks;
            blockNodes[0].previous = 0;
            freeSegments.insert(bTreeFreeSegment(0));
            for (size_t i = 0; i < 256; i++)
                suballocationHeads[i] = nullptr;
        }

        void* allocate(size_t size)
        {
            if (size == 0)
                return nullptr;
            size_t blocksRequested = (size + VAR_POOL_BLOCK_SIZE - 1) / VAR_POOL_BLOCK_SIZE;

            lock.lock();

            void* ptr = nullptr;
            if (size < VAR_ALLOC_THRESHOLD && 0) {
                int id = size_to_alloc_id(size);
                if (id != VAR_ALLOC_ID) 
                    ptr = allocate_fixblock(id);
            }

            if (!ptr) {
                ptr = allocate_varblock(blocksRequested);
                if (ptr) {
                    size_t blockIndex = size_t((uint8_t*)ptr - data) / VAR_POOL_BLOCK_SIZE;
                    blockNodes[blockIndex].allocID = VAR_ALLOC_ID;
                }
            }

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

            size_t blockIndex = size_t((uint8_t*)ptr - data) / VAR_POOL_BLOCK_SIZE;
            // Pointer came from new allocation
            if (blockIndex >= numberBlocks) {
                delete[] (uint8_t*)ptr;
                return;
            }

            lock.lock();

            // Check if this pointer is part of a suballocation inside a 
            // fixed memory pool or if it comes from a variable block allocation.
            if (blockNodes[blockIndex].allocID != VAR_ALLOC_ID)
                deallocate_fixblock(ptr, blockIndex);
            else 
                deallocate_varblock(blockIndex);

            lock.unlock();
        }

        // Returns the ammount of memory used by this pool, that is, the sum of the data blocks and required metadata
        size_t get_internal_allocated_memory()
        {
            size_t usedMemory = VAR_POOL_BLOCK_SIZE * numberBlocks;  // data
            usedMemory += sizeof(VarBlockNode) * numberBlocks;  // block nodes
            usedMemory += bTreeLeafPool.get_internal_allocated_memory();
            usedMemory += bTreeInternalPool.get_internal_allocated_memory();
            return usedMemory;
        }
    };
}

