## Memory pool

A small library/experiment implementing 2 types of memory pools.
- A fixed size pool, where all blocks have the same size. It has very fast allocation.
- A variable size pool, with relatively low overhead for any allocation size.

Unlike other pools these do not dynamically resize; once they are full you will get an std::bad_alloc.

## FixedMemoryPool

Very simple memory pool with fixed size blocks. allocate() and free() are O(1). Memory overhead is 2 bits per block.

For a memory pool with fixed size blocks you only need a stack. The stack is essentially a list of indexes of free blocks. 
- On allocation you pop the top index from it, and use it as your pointer. 
- On deallocation, you insert the index at the top.
 
To reduce memory use, I group blocks into chunks. Each chunk has a bitmap which shows which blocks in it are free. We can quickly find a free block in that bitmap using a bit scan. In this case the stack contains indexes of chunks that are not full. 
- On allocation we take the top chunk from the stack, and remove it ONLY if it becomes full.
- On deallocation we push the chunk into the stack ONLY if it was previously full.

## VariableMemoryPool

A memory pool which can allocate and deallocate any number of bytes with relatively low overhead. allocate() and free() are O(log n), with n the number of allocated segments in the pool.

The pool is composed of blocks of a fixed 1024 bytes. It has 2 different allocation mechanisms depending on the size:
- **>=3072 bytes**: uses a binary tree to quickly find a free segment with enough capacity for the given allocation. Each block has associated information, such as whether is free or occupied, the length of the segment it belongs to and an index pointing to the previous segment.
- **<3072 bytes**: takes 32 contiguous blocks using the previous algorithm, and initializes a FixedMemoryPool in them with an appropiate block size. It uses a linked list to keep track of pools with available blocks for each size. 

The overhead varies depending on the size of the allocation, but is generally low.
![](overhead.png)

## Libraries

B-tree from [parallel-hashmap](https://github.com/greg7mdp/parallel-hashmap) by Gregory Popovitch 