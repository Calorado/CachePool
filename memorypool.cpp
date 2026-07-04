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
#include <string>
#include <random>

#include "memorypool.h"
#include "lib/mimalloc/include/mimalloc.h"
#include "lib/mimalloc/include/mimalloc-stats.h"

struct StdNewAllocator
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
	void restart(size_t) {}
};

struct MimallocAllocator
{
	void* allocate(size_t count) {
		return mi_malloc(count);
	}
	void free(void* ptr) {
		mi_free(ptr);
	}
	size_t get_internal_allocated_memory() {
		mi_stats_t_decl(stats);
		mi_stats_merge();
		mi_stats_get(&stats);
		return stats.committed.current;
	}
	void restart(size_t) {
		mi_collect(true);  // Return all memory to OS
	}
};

template<class Allocator>
void test_overhead(Allocator* allocator, std::string name, std::vector<std::string>* results, size_t poolSize) 
{
	std::vector<void*> pointers;
	pointers.reserve(poolSize);

	for (int i = (1 << 0); i <= (1 << 20); i++) {
		// Don't test all values, otherwise this will take forever
		int log = log2(i);
		if (log >= 11 && (i & (1 << log - 11) - 1) != 0)
			continue;
		allocator->restart(poolSize);

		size_t allocations = 0;
		size_t internalSize = 0;  // Internal size of memory pool/allocator, to calculate overhead
		while (true) {
			try {
				pointers.push_back(allocator->allocate(i));
				memset(pointers.back(), 0, i);
				allocations += 1;

				if (name == "mimalloc") {
					size_t committed = allocator->get_internal_allocated_memory();
					// Last allocation pushed the internally used memory above the target
					if (committed > poolSize) {
						allocations -= 1;
						break;
					}
					internalSize = committed;
				}
			}
			// mypool throws std::bad_alloc when its full
			catch (std::bad_alloc& e) {
				internalSize = allocator->get_internal_allocated_memory();
				break;
			}
		}

		float overhead = (float)internalSize / (allocations * i) - 1;
		std::cout << "\nBlock: " << i << ", Total allocations : " << allocations << ", Overhead : " << overhead;
		results->push_back(std::format("{},{}", i, overhead));

		for (void* ptr : pointers)
			allocator->free(ptr);
		pointers.clear();
	}
}

template<class Allocator>
void test_thread(Allocator* allocator, std::atomic_uint64_t* totalAllocations, size_t maxAllocations, 
	size_t maxAllocationSize, float allocationDistribution, uint32_t seed, bool skipCheck)
{
	uint8_t** allocatedChunks = new uint8_t*[maxAllocations];
	memset(allocatedChunks, 0, sizeof(uint8_t*) * maxAllocations);
	size_t* allocatedSizes = new size_t[maxAllocations];
	memset(allocatedSizes, 0, sizeof(size_t) * maxAllocations);

	std::mt19937 generator(seed);
	std::uniform_int_distribution<> indexDistribution(0, maxAllocations - 1);
	std::uniform_int_distribution<> uniformDistribution(1, maxAllocationSize);
	std::exponential_distribution<> exponentialDistribution(allocationDistribution);

	while (true) {
		uint64_t chunkIdx = indexDistribution(generator);
		if (allocatedChunks[chunkIdx]) 
		{
			if (!skipCheck) {
				size_t expectedID = 1 + chunkIdx % 255;
				size_t count = std::count(allocatedChunks[chunkIdx], allocatedChunks[chunkIdx] + allocatedSizes[chunkIdx], expectedID);
				if (count != allocatedSizes[chunkIdx]) {
					std::cout << "\nERROR: out of bounds write detected";
					exit(-1);
				}
				memset(allocatedChunks[chunkIdx], 0, allocatedSizes[chunkIdx]);
			}
			allocator->free(allocatedChunks[chunkIdx]);
			allocatedChunks[chunkIdx] = nullptr;
			allocatedSizes[chunkIdx] = 0;
		}
		else 
		{
			uint64_t chunkSize;
			if (allocationDistribution == 0)
				chunkSize = uniformDistribution(generator);
			else
				chunkSize = (size_t)std::min(exponentialDistribution(generator), (double)maxAllocationSize);
			uint8_t* ptr;
			try {
				ptr = (uint8_t*)allocator->allocate(chunkSize);
			}
			catch (std::bad_alloc& e) {
				std::cout << "\nERROR: std::bad_alloc";
				exit(-1);
			}
			allocatedChunks[chunkIdx] = ptr;
			allocatedSizes[chunkIdx] = chunkSize;
			if (!skipCheck)
				memset(ptr, 1 + chunkIdx % 255, chunkSize);
		}

		totalAllocations->fetch_add(1);
	}
}

int main(int argc, char* argv[])
{
	if (argc < 3) {
		std::cout << "Usage: memorypool [OPTIONS...] [TEST_TYPE] [ALLOCATOR]" << "\n";
		std::cout << "Allocators" << "\n";
		std::cout << "stdnew: use new[] and delete[]." << "\n";
		std::cout << "mimalloc: use Microsofts mimalloc library." << "\n";
		std::cout << "mypool: use the memory pool in this library." << "\n";
		std::cout << "Test types" << "\n";
		std::cout << "overhead: calculate memory overhead for different allocation sizes. Outputs results to overhead.csv. Only implemented for mypool." << "\n";
		std::cout << "fuzz: test that the allocator works correctly." << "\n";
		std::cout << "benchmark: test number of allocation and deallocations in 10 seconds." << "\n";
		std::cout << "Options" << "\n";
		std::cout << "-t: number of threads. Default 1." << "\n";
		std::cout << "-d: allocation sizes follow an exponential distribution. This sets the constant. Default 0 (uniform)." << "\n";
		std::cout << "-a: maximum allocation size in bytes. Default 100000." << "\n";
		std::cout << "-c: maximum number of allocations. Default 2048." << "\n";
		std::cout << "-p: size of the pool. Default 250000000." << "\n";
		std::cout << "-s: seed for random number generator. Default random." << "\n";
		exit(0);
	}

	std::string test = argv[argc - 2];
	std::string allocator = argv[argc - 1];
	size_t threadCount = 1;
	size_t maxAllocationSize = 100000;
	size_t maxAllocations = 2048;
	float allocationDistribution = 0;
	size_t poolSize = 250000000;
	uint32_t seed = -1;

	for (int i = 1; i < argc - 2; i += 2) {
		if (argv[i][1] == 't')
			threadCount = std::stoi(argv[i + 1]);
		else if (argv[i][1] == 'd')
			allocationDistribution = std::stof(argv[i + 1]);
		else if (argv[i][1] == 'a')
			maxAllocationSize = std::stoll(argv[i + 1]);
		else if (argv[i][1] == 'c')
			maxAllocations = std::stoll(argv[i + 1]);
		else if (argv[i][1] == 'p')
			poolSize = std::stoll(argv[i + 1]);
		else if (argv[i][1] == 's')
			seed = std::stoi(argv[i + 1]);
		else {
			std::cout << "\nUnknown option " << argv[i];
			exit(0);
		}
	}

	if (seed == (uint32_t)-1) {
		std::random_device device;
		seed = device();
	}
	std::cout << "\nUsing seed " << seed;

	if (test == "fuzz" || test == "benchmark") 
	{
		bool skipCheck = test == "benchmark";
		std::thread* threads = new std::thread[threadCount];
		std::atomic_uint64_t totalAllocations = 0;

		StdNewAllocator stdNewAllocator = StdNewAllocator();
		MimallocAllocator mimallocAllocator = MimallocAllocator();
		mempool::variablepool::VariableMemoryPool<std::mutex> mypoolAllocator = mempool::variablepool::VariableMemoryPool<std::mutex>();

		if (allocator == "stdnew") {
			for (int i = 0; i < threadCount; i++) 
				threads[i] = std::thread(test_thread<StdNewAllocator>, &stdNewAllocator, &totalAllocations,
					maxAllocations, maxAllocationSize, allocationDistribution, seed, skipCheck);
		}
		else if (allocator == "mimalloc") {
			for (int i = 0; i < threadCount; i++)
				threads[i] = std::thread(test_thread<MimallocAllocator>, &mimallocAllocator, &totalAllocations,
					maxAllocations, maxAllocationSize, allocationDistribution, seed, skipCheck);
		}
		else if (allocator == "mypool") {
			mypoolAllocator.restart(poolSize);
			for (int i = 0; i < threadCount; i++)
				threads[i] = std::thread(test_thread<mempool::variablepool::VariableMemoryPool<std::mutex>>, &mypoolAllocator,
					&totalAllocations, maxAllocations, maxAllocationSize, allocationDistribution, seed, skipCheck);
		}
		else {
			std::cout << "\nUnkown allocator " << allocator;
			exit(0);
		}

		std::cout << "\n";
		auto startTime = std::chrono::high_resolution_clock::now();
		
		while (true) {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			auto currentTime = std::chrono::high_resolution_clock::now();
			auto elapsed = (currentTime - startTime).count() / 1e9;
			std::cout << std::string(100, '\b');
			std::cout << "Elapsed " << elapsed << "s | Allocations: " << (float)totalAllocations.load() / elapsed << "/s" << "        ";
			if (test == "benchmark" && elapsed > 10)
				break;
		}
		exit(0);
	}
	else if (test == "overhead")
	{
		std::vector<std::string> results;
		mempool::variablepool::VariableMemoryPool<std::mutex> mypoolAllocator = mempool::variablepool::VariableMemoryPool<std::mutex>();
		MimallocAllocator mimallocAllocator = MimallocAllocator();

		if (allocator == "mypool") {
			mypoolAllocator.restart(poolSize);
			std::cout << "\nMemory used by pool: " << mypoolAllocator.get_internal_allocated_memory();
			test_overhead<mempool::variablepool::VariableMemoryPool<std::mutex>>(&mypoolAllocator, allocator, &results, poolSize);
		}
		else if (allocator == "mimalloc")
			test_overhead<MimallocAllocator>(&mimallocAllocator, allocator, &results, poolSize);
		else {
			std::cout << "\nUnimplemented allocator " << allocator;
			exit(0);
		}

		std::fstream resultFile = std::fstream("overhead.csv", std::fstream::out);
		resultFile << "block size,overhead\n";
		for (std::string result : results)
			resultFile << result << "\n";
		resultFile.close();
		exit(0);
	}
	else {
		std::cout << "\nUnkown test type " << test;
		exit(0);
	}
}
