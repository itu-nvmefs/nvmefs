#include "nvmefs_temporary_block_manager.hpp"
#include <optional>
#include <atomic>

namespace duckdb {

std::atomic<uint64_t> nvmefs_active_temp_bytes {0};
std::atomic<uint64_t> nvmefs_peak_temp_bytes {0};

TemporaryBlock::TemporaryBlock(idx_t start_lba, idx_t lba_count)
    : start_lba(start_lba), lba_count(lba_count), is_free(false) {
	next_block = nullptr;
	previous_block = nullptr;
	next_free_block = nullptr;
	previous_free_block = nullptr;
}

idx_t TemporaryBlock::GetLBACount() {
	return lba_count;
}

idx_t TemporaryBlock::GetStartLBA() {
	return start_lba;
}

idx_t TemporaryBlock::GetEndLBA() {
	return start_lba + lba_count - 1;
}

bool TemporaryBlock::IsFree() {
	return is_free;
}

NvmeTemporaryBlockManager::NvmeTemporaryBlockManager(idx_t allocated_lba_start, idx_t allocated_lba_end)
    : allocated_start_lba(allocated_lba_start), allocated_end_lba(allocated_lba_end) {
	// Initialize the linked list of free blocks
	blocks = make_uniq<TemporaryBlock>(allocated_lba_start, allocated_lba_end - allocated_lba_start);
	blocks_free =
	    vector<TemporaryBlock *>(8, nullptr); // There are 8 different allocation sizes for the TemporaryBufferSize

	blocks_free[7] = blocks.get(); // The largest block is the first one
	blocks->is_free = true;        // Mark the block as free
}

uint8_t NvmeTemporaryBlockManager::GetFreeListIndex(idx_t lba_count) {
	// Get the index of the free list for the given size
	if (lba_count <= 8) {
		return 0;
	} else if (lba_count <= 16) {
		return 1;
	} else if (lba_count <= 24) {
		return 2;
	} else if (lba_count <= 32) {
		return 3;
	} else if (lba_count <= 40) {
		return 4;
	} else if (lba_count <= 48) {
		return 5;
	} else if (lba_count <= 56) {
		return 6;
	} else if (lba_count <= 64) {
		return 7;
	}

	return 7;
}

TemporaryBlock *NvmeTemporaryBlockManager::AllocateBlock(idx_t lba_count) {
	// auto start_time = std::chrono::high_resolution_clock::now();
	// Get the free list index for the given size
	uint8_t free_list_index = GetFreeListIndex(lba_count);

	TemporaryBlock *block = nullptr;

	// Get the free block from the free list
	for (uint8_t i = free_list_index; i < 8; i++) {
		if (blocks_free[i] != nullptr) {
			// Get the block from the free list
			block = PopFreeBlock(i);

			// Check if the block is large enough
			// Split the block if it is larger than the requested size
			if (block->lba_count > lba_count) {
				block = SplitBlock(block, lba_count);
			}

			break; // If we are in here we have found a block
		}
	}

	if (block == nullptr) {
		throw std::runtime_error("No free block available");
	}

	// Return the block
	block->is_free = false; // Mark the block as used

	uint64_t added_bytes = lba_count * 4096; // Assuming 4K LBA
	uint64_t current_bytes = nvmefs_active_temp_bytes.fetch_add(added_bytes) + added_bytes;

	uint64_t peak = nvmefs_peak_temp_bytes.load();
	while (current_bytes > peak && !nvmefs_peak_temp_bytes.compare_exchange_weak(peak, current_bytes)) {
	}

	// auto end_time = std::chrono::high_resolution_clock::now();
	// auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
	// // Print the duration
	// printf("AllocateBlock took %d milliseconds.\n", duration.count());

	return block;
}

void NvmeTemporaryBlockManager::PrintBlocks(TemporaryBlock *block) {
	printf("-------\n");
	while (block != nullptr) {
		printf("Block start lba %llu end lba %llu, is_free: %d\n", block->GetStartLBA(), block->GetEndLBA(),
		       block->IsFree());
		block = block->next_block.get();
	}
	printf("-------\n");
}
void NvmeTemporaryBlockManager::PrintBlocksBackwards(TemporaryBlock *block) {
	printf("-------\n");
	while (block != nullptr) {
		printf("Block start lba %llu end lba %llu, is_free %d\n", block->GetStartLBA(), block->GetEndLBA(),
		       block->IsFree());
		block = block->previous_block;
	}
	printf("-------\n");
}

TemporaryBlock *NvmeTemporaryBlockManager::SplitBlock(TemporaryBlock *block, idx_t lba_count) {
	// Create a new block with the remaining size
	unique_ptr<TemporaryBlock> new_block = make_uniq<TemporaryBlock>(block->start_lba, lba_count);
	TemporaryBlock *new_block_ptr = new_block.get();

	// Update the original block size
	idx_t endLba = block->GetEndLBA();
	block->start_lba += lba_count;
	block->lba_count -= lba_count;

	// Add the new block to the free list
	if (new_block->GetStartLBA() != allocated_start_lba) {
		auto prev = block->previous_block;
		auto old_block = move(prev->next_block);

		new_block->previous_block = prev;        // Set the previous block to the new split block
		new_block->next_block = move(old_block); // Get current blocks unique_ptr and move it
		prev->next_block = move(new_block);      // Move the new block to the previous block
	} else {
		new_block->next_block = move(blocks); // Move the new block to the head of the list
		blocks = move(new_block);             // Move the new block to the head of the list
	}

	block->previous_block = new_block_ptr; // Set the previous block to the new split block

	block->is_free = true;
	PushFreeBlock(block); // Add the block to the free list
	return new_block_ptr;
}

void NvmeTemporaryBlockManager::FreeBlock(TemporaryBlock *block) {
	// auto start_time = std::chrono::high_resolution_clock::now();

	// Mark the block as free
	block->is_free = true;
	nvmefs_active_temp_bytes.fetch_sub(block->lba_count * 4096); // Assuming 4K LBA

	// Coalesce the free blocks
	CoalesceFreeBlocks(block);

	// Add the block to the free list
	PushFreeBlock(block);

	// auto end_time = std::chrono::high_resolution_clock::now();
	// auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
	// // Print the duration
	// printf("FreeBlock took %d milliseconds.\n", duration.count());
}

void NvmeTemporaryBlockManager::PushFreeBlock(TemporaryBlock *block) {
	D_ASSERT(block->IsFree());
	D_ASSERT(block->next_free_block == nullptr);
	D_ASSERT(block->previous_free_block == nullptr);

	// Add the block to the free list
	uint8_t free_list_index = GetFreeListIndex(block->lba_count);

	TemporaryBlock *previous_top_block = blocks_free[free_list_index];
	if (previous_top_block != nullptr) {
		previous_top_block->previous_free_block = block; // Set the previous block to the new free block
	}

	// Add the block to be the new top of the free list
	blocks_free[free_list_index] = block;
	block->next_free_block = previous_top_block; // Attach the provious top block to the new top block
}

TemporaryBlock *NvmeTemporaryBlockManager::PopFreeBlock(uint8_t free_list_index) {
	// Get the block from the free list
	TemporaryBlock *popped_block = blocks_free[free_list_index];

	D_ASSERT(popped_block != nullptr);
	D_ASSERT(popped_block->IsFree());

	TemporaryBlock *new_head_block = popped_block->next_free_block;

	// Remove the block from the free list
	blocks_free[free_list_index] = new_head_block;
	popped_block->is_free = false; // Mark the block as free

	// Clean the free block pointers
	popped_block->next_free_block = nullptr;
	popped_block->previous_free_block = nullptr;

	if (new_head_block != nullptr) {
		new_head_block->previous_free_block = nullptr; // Set the previous block to null
	}

	return popped_block;
}

void NvmeTemporaryBlockManager::RemoveFreeBlock(TemporaryBlock *block) {
	if (block->next_free_block != nullptr) {
		block->next_free_block->previous_free_block = block->previous_free_block;
	}

	if (block->previous_free_block == nullptr) {
		uint8_t free_list_index = GetFreeListIndex(block->lba_count);

		blocks_free[free_list_index] = block->next_free_block;
		if (blocks_free[free_list_index] != nullptr) {
			blocks_free[free_list_index]->previous_free_block = nullptr; // Set the previous block to null
		}
	} else {
		// Set the previous free block to the next free block
		block->previous_free_block->next_free_block = block->next_free_block;
	}

	block->next_free_block = nullptr;
	block->previous_free_block = nullptr;
}

void NvmeTemporaryBlockManager::CoalesceFreeBlocks(TemporaryBlock *block) {
	// Check if the previous block is free
	if ((block->previous_block != nullptr && block->previous_block->IsFree()) &&
	    (block->next_block != nullptr && block->next_block->IsFree())) {
		block->start_lba = block->previous_block->start_lba; // Set the start lba to the previous blocks start lba
		block->lba_count += block->previous_block->lba_count + block->next_block->lba_count;

		if (block->previous_block->previous_block != nullptr) {
			unique_ptr<TemporaryBlock> old_left_block = move(block->previous_block->previous_block->next_block);

			block->previous_block->previous_block->next_block =
			    move(block->previous_block->next_block); // Move the previous block to the new merged block

			block->previous_block =
			    block->previous_block->previous_block; // Set the previous block to the new merged block

			RemoveFreeBlock(old_left_block.get()); // Remove the previous block from the free list
		} else {
			RemoveFreeBlock(blocks.get()); // Remove the previous block from the free list
			blocks = move(block->previous_block->next_block);
			blocks->previous_block = nullptr; // Set the previous block to null
		}

		// Merge the next block
		unique_ptr<TemporaryBlock> old_right_block = move(block->next_block);

		// Merge the next block
		if (old_right_block->next_block != nullptr) {
			old_right_block->next_block->previous_block = block; // Move the next block to the new merged block
			block->next_block = move(old_right_block->next_block);
		}

		RemoveFreeBlock(old_right_block.get()); // Remove the next block from the free list

	} else if (block->previous_block != nullptr && block->previous_block->IsFree()) {
		block->start_lba = block->previous_block->start_lba; // Set the start lba to the previous blocks start lba
		block->lba_count += block->previous_block->lba_count;

		if (block->previous_block->previous_block != nullptr) {
			unique_ptr<TemporaryBlock> old_left_block = move(block->previous_block->previous_block->next_block);

			block->previous_block->previous_block->next_block =
			    move(block->previous_block->next_block); // Move the previous block to the new merged block

			block->previous_block =
			    block->previous_block->previous_block; // Set the previous block to the new merged block

			RemoveFreeBlock(old_left_block.get()); // Remove the previous block from the free list
		} else {
			RemoveFreeBlock(blocks.get()); // Remove the previous block from the free list
			blocks = move(block->previous_block->next_block);
			blocks->previous_block = nullptr; // Set the previous block to null
		}
	} else if (block->next_block != nullptr && block->next_block->IsFree()) {
		block->lba_count += block->next_block->lba_count;

		unique_ptr<TemporaryBlock> old_right_block = move(block->next_block);

		// Merge the next block
		if (old_right_block->next_block != nullptr) {
			old_right_block->next_block->previous_block = block; // Move the next block to the new merged block
			block->next_block = move(old_right_block->next_block);
		}

		RemoveFreeBlock(old_right_block.get()); // Remove the next block from the free list
	}
}

} // namespace duckdb
