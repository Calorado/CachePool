/*
Copyright (c) 2026 Carlos de Diego

This Source Code Form is subject to the terms of the
Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <vector>
#include <fstream>
#include <format>
#include <cmath>
#include <algorithm>

#include "memorypool.h"

const size_t NUMBER_THREADS = 1;// std::thread::hardware_concurrency();
const size_t MAX_ALLOCATED_CHUNKS = 4096;  // Per thread
const size_t MAX_CHUNK_SIZE = 1 << 20;
const size_t POOL_SIZE = MAX_ALLOCATED_CHUNKS * MAX_CHUNK_SIZE * NUMBER_THREADS;
const bool SKIP_BUFFER_CHECK = true;  // If true do not check if the buffer was overwritten. Useful for testing allocation speed.

struct StdAllocMemoryPool
{
	void* allocate(size_t count) {
		return (void*)(new char[count]);
	}
	void free(void* ptr) {
		delete[] ptr;
	}
	size_t get_internal_allocated_memory() {
		return 0;
	}
	void restart(size_t, bool) {}
};

uint64_t random(uint64_t* value) {
	*value ^= *value >> 33;
	*value *= 0xff51afd7ed558ccd;
	*value ^= *value >> 33;
	*value += 0xc4ceb9fe1a85ec53;
	*value ^= *value >> 33;
	return *value;
}

mempool::variablepool::VariableMemoryPool<std::mutex> memoryPool = mempool::variablepool::VariableMemoryPool<std::mutex>(POOL_SIZE, false);
//StdAllocMemoryPool memoryPool = StdAllocMemoryPool();
std::atomic_uint64_t totalAllocations;

size_t find_maximum_allocation(size_t blockSize) 
{
	size_t allocations = 0;
	while (true) {
		try {
			memoryPool.allocate(blockSize);
			allocations += 1;
		}
		catch (std::bad_alloc& e) {
			return allocations;
		}
	}
}

void testing_thread(int id) 
{
	uint8_t* allocatedChunks[MAX_ALLOCATED_CHUNKS];
	memset(allocatedChunks, 0, sizeof(uint8_t*) * MAX_ALLOCATED_CHUNKS);
	size_t allocatedSizes[MAX_ALLOCATED_CHUNKS];
	uint64_t seed = id;

	while (true) {
		uint64_t chunkIdx = random(&seed) % MAX_ALLOCATED_CHUNKS;
		if (allocatedChunks[chunkIdx]) {
			if (!SKIP_BUFFER_CHECK) {
				size_t expectedID = 1 + chunkIdx % 255;
				size_t count = std::count(allocatedChunks[chunkIdx], allocatedChunks[chunkIdx] + allocatedSizes[chunkIdx], expectedID);
				if (count != allocatedSizes[chunkIdx]) {
					std::cout << "\nERROR: out of bounds write detected, ID " << expectedID;
					exit(-1);
				}
				memset(allocatedChunks[chunkIdx], 0, allocatedSizes[chunkIdx]);
			}

			memoryPool.free(allocatedChunks[chunkIdx]);
			allocatedChunks[chunkIdx] = nullptr;
		}
		else {
			uint64_t chunkSize = random(&seed) % MAX_CHUNK_SIZE + 1;
			uint8_t* ptr = (uint8_t*)memoryPool.allocate(chunkSize);
			allocatedChunks[chunkIdx] = ptr;
			allocatedSizes[chunkIdx] = chunkSize;
			if (!SKIP_BUFFER_CHECK)
				memset(ptr, 1 + chunkIdx % 255, chunkSize);
		}
		totalAllocations.fetch_add(1);
	}
}

int main()
{
	if (0) {
		std::cout << "\nMemory used by pool: " << memoryPool.get_internal_allocated_memory();

		std::vector<std::string> results;
		for (int i = 1; i <= 1048576; i++) { 
			int log = log2(i);
			if (log >= 12 && (i & (1 << log - 12) - 1) != 0)
				continue;
			memoryPool.restart(POOL_SIZE, false);
			size_t allocations = find_maximum_allocation(i);
			float overhead = (float)memoryPool.get_internal_allocated_memory() / (allocations * i) - 1;
			std::cout << "\nBlock: " << i << ", Total allocations : " << allocations << ", Overhead : " << overhead;
			results.push_back(std::format("{},{}", i, overhead));
		}

		std::fstream resultFile = std::fstream("overhead.csv", std::fstream::out);
		resultFile << "block size,overhead\n";
		for (std::string result : results)
			resultFile << result << "\n";
		resultFile.close();
		exit(0);
	}

	if (1) {
		auto startTime = std::chrono::high_resolution_clock::now();
		std::thread* threads = new std::thread[NUMBER_THREADS];
		for (int i = 0; i < NUMBER_THREADS; i++)
			threads[i] = std::thread(testing_thread, i);

		while (true) {
			auto currentTime = std::chrono::high_resolution_clock::now();
			std::cout << "\nAllocations: " << (float)totalAllocations.load() / (currentTime - startTime).count() * 1e9 << "/s";
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
	}
}
