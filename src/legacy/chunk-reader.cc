// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Chunked data reader implementation

#include "chunk-reader.hh"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <chrono>

namespace lightusd {

ChunkReader::ChunkReader(size_t total_size, const Config& config)
    : config_(config), total_size_(total_size), current_offset_(0),
      current_sliding_window_size_(0), current_random_cache_size_(0), current_preload_size_(0),
      last_accessed_chunk_(SIZE_MAX), sequential_access_count_(0), in_sequential_mode_(false),
      global_timestamp_(0) {
  // Validate configuration
  if (config_.chunk_size == 0) {
    config_.chunk_size = 1024 * 1024; // 1MB default
  }

  if (config_.max_chunks == 0) {
    config_.max_chunks = 16;
  }

  if (config_.max_buffer_size == 0) {
    config_.max_buffer_size = 16 * 1024 * 1024; // 16MB default
  }

  // Ensure max_buffer_size is at least one chunk
  if (config_.max_buffer_size < config_.chunk_size) {
    config_.max_buffer_size = config_.chunk_size;
  }

  // Calculate actual max chunks based on buffer size
  size_t max_chunks_by_buffer = config_.max_buffer_size / config_.chunk_size;
  if (max_chunks_by_buffer < config_.max_chunks) {
    config_.max_chunks = max_chunks_by_buffer;
  }

  // Validate allocation ratios
  float total_ratio = config_.sliding_window_ratio + config_.random_cache_ratio + config_.preload_ratio;
  if (total_ratio > 100.1f || total_ratio < 99.9f) {
    // Normalize ratios if they don't sum to 100
    config_.sliding_window_ratio = (config_.sliding_window_ratio / total_ratio) * 100.0f;
    config_.random_cache_ratio = (config_.random_cache_ratio / total_ratio) * 100.0f;
    config_.preload_ratio = (config_.preload_ratio / total_ratio) * 100.0f;
  }

  // Initialize cache sizes and structures
  InitializeCacheSizes();
}

ChunkReader::~ChunkReader() {
  // Cleanup is automatic with smart pointers
}

nonstd::expected<ChunkReader::ReadResult, std::string> ChunkReader::Read(size_t offset, size_t size, uint8_t* buffer) {
  ReadResult result;
  result.fully_cached = true;
  result.bytes_available = 0;

  // Error checking
  if (offset >= total_size_) {
    return nonstd::make_unexpected("Read offset " + std::to_string(offset) +
                                   " exceeds EOF (" + std::to_string(total_size_) + ")");
  }

  // Check if read size exceeds maximum allowed
  size_t max_read_size = config_.chunk_size * config_.max_chunks;
  if (size > max_read_size) {
    return nonstd::make_unexpected("Read size " + std::to_string(size) +
                                   " exceeds maximum allowed (" + std::to_string(max_read_size) + ")");
  }

  // Adjust size if it goes beyond EOF
  size_t actual_size = std::min(size, total_size_ - offset);

  // Calculate which chunks are needed
  size_t start_chunk = GetChunkId(offset);
  size_t end_offset = offset + actual_size - 1;
  size_t end_chunk = GetChunkId(end_offset);

  // Check which chunks are not in cache
  {
    MutexLockGuard lock(cache_mutex_);

    for (size_t chunk_id = start_chunk; chunk_id <= end_chunk; ++chunk_id) {
      // Check caches directly without calling IsChunkCached to avoid mutex issues
      bool cached = false;

      if (sliding_window_ && sliding_window_->contains(chunk_id)) {
        cached = true;
      } else if (random_cache_.find(chunk_id) != random_cache_.end()) {
        cached = true;
      } else if (preload_cache_.find(chunk_id) != preload_cache_.end()) {
        cached = true;
      }

      if (!cached) {
        result.required_chunks.push_back(chunk_id);
        result.fully_cached = false;
      }
    }

    // Calculate bytes available from cache
    if (!result.fully_cached) {
      // Find contiguous cached chunks from the start
      for (size_t chunk_id = start_chunk; chunk_id <= end_chunk; ++chunk_id) {
        bool cached = false;

        if (sliding_window_ && sliding_window_->contains(chunk_id)) {
          cached = true;
        } else if (random_cache_.find(chunk_id) != random_cache_.end()) {
          cached = true;
        } else if (preload_cache_.find(chunk_id) != preload_cache_.end()) {
          cached = true;
        }

        if (cached) {
          size_t chunk_start = chunk_id * config_.chunk_size;
          size_t chunk_end = std::min(chunk_start + config_.chunk_size, total_size_);

          if (chunk_id == start_chunk) {
            // First chunk - may be partial
            size_t offset_in_chunk = GetChunkOffset(offset);
            result.bytes_available += std::min(chunk_end - chunk_start - offset_in_chunk, actual_size);
          } else if (result.bytes_available > 0) {
            // Only count if contiguous with previous chunks
            size_t prev_end = offset + result.bytes_available;
            if (prev_end >= chunk_start) {
              result.bytes_available += std::min(chunk_end - chunk_start, actual_size - result.bytes_available);
            } else {
              break; // Not contiguous
            }
          }
        } else {
          break; // Missing chunk breaks contiguity
        }
      }
    } else {
      result.bytes_available = actual_size;
    }
  }

  // If buffer is provided and all data is cached, perform the read
  if (buffer && result.fully_cached) {
    auto read_result = ReadDirect(offset, size, buffer, false);
    if (!read_result) {
      return nonstd::make_unexpected(read_result.error());
    }
  }

  // Update statistics
  stats_.total_reads++;
  if (result.fully_cached) {
    stats_.cache_hits++;
  } else {
    stats_.cache_misses++;
  }

  return result;
}

nonstd::expected<size_t, std::string> ChunkReader::ReadDirect(size_t offset, size_t size, uint8_t* buffer, bool force_load) {
  if (!buffer) {
    return nonstd::make_unexpected("Buffer is null");
  }

  if (offset >= total_size_) {
    return nonstd::make_unexpected("Offset beyond end of stream");
  }

  // Adjust size if it goes beyond the stream
  size_t actual_size = std::min(size, total_size_ - offset);
  size_t bytes_read = 0;

  // Read data that may span multiple chunks
  while (bytes_read < actual_size) {
    size_t current_offset = offset + bytes_read;
    size_t chunk_id = GetChunkId(current_offset);
    size_t offset_in_chunk = GetChunkOffset(current_offset);

    // Get the chunk (from cache or load it if force_load is true)
    std::shared_ptr<Chunk> chunk;

    if (force_load) {
      auto chunk_result = GetChunk(chunk_id);
      if (!chunk_result) {
        return nonstd::make_unexpected(chunk_result.error());
      }
      chunk = chunk_result.value();
    } else {
      // Only use cached chunks
      MutexLockGuard lock(cache_mutex_);

      // Try to find chunk in any cache
      chunk = FindInSlidingWindow(chunk_id);
      if (!chunk) {
        chunk = FindInRandomCache(chunk_id);
      }
      if (!chunk) {
        chunk = FindInPreloadCache(chunk_id);
      }

      if (!chunk) {
        // Chunk not in cache and force_load is false
        break;
      }
    }

    // Calculate how much to read from this chunk. Guard the subtraction: a
    // corrupt offset > chunk->size would underflow to a huge value and feed an
    // out-of-bounds memcpy. Offsets are validated upstream, but this is cheap
    // defense-in-depth.
    if (offset_in_chunk > chunk->size) {
      break;
    }
    size_t remaining_in_chunk = chunk->size - offset_in_chunk;
    size_t to_read = std::min(remaining_in_chunk, actual_size - bytes_read);

    // Copy data from chunk to buffer
    std::memcpy(buffer + bytes_read, chunk->data.data() + offset_in_chunk, to_read);
    bytes_read += to_read;

    // Update statistics
    stats_.bytes_read += to_read;
  }

  // Update current position
  current_offset_ = offset + bytes_read;

  // Prefetch next chunks if enabled
  if (config_.enable_prefetch && bytes_read > 0 && force_load) {
    Prefetch(current_offset_, config_.chunk_size * config_.prefetch_distance);
  }

  return bytes_read;
}

nonstd::expected<bool, std::string> ChunkReader::ReadByte(size_t offset, uint8_t* value) {
  if (!value) {
    return nonstd::make_unexpected("Value pointer is null");
  }

  auto result = ReadDirect(offset, 1, value);
  if (!result) {
    return nonstd::make_unexpected(result.error());
  }

  if (result.value() != 1) {
    return nonstd::make_unexpected("Failed to read byte");
  }

  return true;
}

nonstd::expected<bool, std::string> ChunkReader::Read2(size_t offset, uint16_t* value) {
  if (!value) {
    return nonstd::make_unexpected("Value pointer is null");
  }

  uint8_t buffer[2];
  auto result = ReadDirect(offset, 2, buffer);
  if (!result) {
    return nonstd::make_unexpected(result.error());
  }

  if (result.value() != 2) {
    return nonstd::make_unexpected("Failed to read 2 bytes");
  }

  // Assume little-endian for now (can be made configurable)
  *value = uint16_t(static_cast<uint16_t>(buffer[0]) |
           (static_cast<uint16_t>(buffer[1]) << 8));

  return true;
}

nonstd::expected<bool, std::string> ChunkReader::Read4(size_t offset, uint32_t* value) {
  if (!value) {
    return nonstd::make_unexpected("Value pointer is null");
  }

  uint8_t buffer[4];
  auto result = ReadDirect(offset, 4, buffer);
  if (!result) {
    return nonstd::make_unexpected(result.error());
  }

  if (result.value() != 4) {
    return nonstd::make_unexpected("Failed to read 4 bytes");
  }

  // Assume little-endian for now
  *value = static_cast<uint32_t>(buffer[0]) |
           (static_cast<uint32_t>(buffer[1]) << 8) |
           (static_cast<uint32_t>(buffer[2]) << 16) |
           (static_cast<uint32_t>(buffer[3]) << 24);

  return true;
}

nonstd::expected<bool, std::string> ChunkReader::Read8(size_t offset, uint64_t* value) {
  if (!value) {
    return nonstd::make_unexpected("Value pointer is null");
  }

  uint8_t buffer[8];
  auto result = ReadDirect(offset, 8, buffer);
  if (!result) {
    return nonstd::make_unexpected(result.error());
  }

  if (result.value() != 8) {
    return nonstd::make_unexpected("Failed to read 8 bytes");
  }

  // Assume little-endian for now
  *value = static_cast<uint64_t>(buffer[0]) |
           (static_cast<uint64_t>(buffer[1]) << 8) |
           (static_cast<uint64_t>(buffer[2]) << 16) |
           (static_cast<uint64_t>(buffer[3]) << 24) |
           (static_cast<uint64_t>(buffer[4]) << 32) |
           (static_cast<uint64_t>(buffer[5]) << 40) |
           (static_cast<uint64_t>(buffer[6]) << 48) |
           (static_cast<uint64_t>(buffer[7]) << 56);

  return true;
}

bool ChunkReader::Seek(size_t offset) {
  if (offset > total_size_) {
    return false;
  }
  current_offset_ = offset;
  return true;
}

void ChunkReader::Prefetch(size_t offset, size_t size) {
  // Calculate which chunks to prefetch
  size_t start_chunk = GetChunkId(offset);
  size_t end_offset = std::min(offset + size, total_size_);
  size_t end_chunk = GetChunkId(end_offset);

  std::unique_lock<Mutex> lock(cache_mutex_);

  // Prefetch chunks into preload cache
  for (size_t chunk_id = start_chunk; chunk_id <= end_chunk; ++chunk_id) {
    // Skip if already cached anywhere
    if (IsChunkCached(chunk_id)) {
      continue;
    }

    // Load chunk for preload cache
    lock.unlock();
    auto chunk_result = LoadChunk(chunk_id);
    lock.lock();

    // Another thread may have populated the cache while the mutex was
    // released; skip inserting so we don't duplicate an entry.
    if (chunk_result && !IsChunkCached(chunk_id)) {
      InsertIntoPreloadCache(chunk_id, chunk_result.value());
    }
  }
}

void ChunkReader::ClearCache() {
  MutexLockGuard lock(cache_mutex_);

  // Clear sliding window
  if (sliding_window_) {
    sliding_window_->chunks.assign(sliding_window_->capacity, nullptr);
    sliding_window_->head = 0;
    sliding_window_->tail = 0;
    sliding_window_->size = 0;
    sliding_window_->base_chunk_id = 0;
  }

  // Clear random cache
  random_cache_.clear();
  random_cache_lru_.clear();
  if (two_q_cache_) {
    two_q_cache_->a1_fifo.clear();
    two_q_cache_->am_lru.clear();
  }
  if (tiny_lfu_sketch_) {
    tiny_lfu_sketch_->sketch.assign(tiny_lfu_sketch_->size, 0);
    tiny_lfu_sketch_->total_count = 0;
  }

  // Clear preload cache
  preload_cache_.clear();
  preload_lru_.clear();

  // Reset sizes
  current_sliding_window_size_ = 0;
  current_random_cache_size_ = 0;
  current_preload_size_ = 0;
}

ChunkReader::Stats ChunkReader::GetStats() const {
  return stats_;
}

void ChunkReader::ResetStats() {
  stats_ = Stats();
}

bool ChunkReader::IsChunkCached(size_t chunk_id) const {
  // Note: This method should NOT acquire the mutex if called from within
  // a method that already holds it. For now, we'll make it work safely.

  // Check sliding window
  if (sliding_window_ && sliding_window_->contains(chunk_id)) {
    return true;
  }

  // Check random cache
  if (random_cache_.find(chunk_id) != random_cache_.end()) {
    return true;
  }

  // Check preload cache
  if (preload_cache_.find(chunk_id) != preload_cache_.end()) {
    return true;
  }

  return false;
}

size_t ChunkReader::GetCacheSize() const {
  MutexLockGuard lock(cache_mutex_);
  return current_sliding_window_size_ + current_random_cache_size_ + current_preload_size_;
}

size_t ChunkReader::GetCachedChunkCount() const {
  MutexLockGuard lock(cache_mutex_);
  size_t total = 0;

  if (sliding_window_) {
    total += sliding_window_->size;
  }

  total += random_cache_.size();
  total += preload_cache_.size();

  return total;
}

nonstd::expected<std::shared_ptr<ChunkReader::Chunk>, std::string>
ChunkReader::LoadChunk(size_t chunk_id) {
  // Calculate chunk parameters
  size_t chunk_offset = chunk_id * config_.chunk_size;
  if (chunk_offset >= total_size_) {
    return nonstd::make_unexpected("Chunk offset beyond stream size");
  }

  size_t chunk_size = std::min(config_.chunk_size, total_size_ - chunk_offset);

  // Create new chunk
  auto chunk = std::make_shared<Chunk>(chunk_id, chunk_offset, chunk_size);

  // Call the callback to load data
  if (!chunk_request_callback_) {
    return nonstd::make_unexpected("No chunk request callback set");
  }

  if (!chunk_request_callback_(chunk_id, chunk_offset, chunk_size, chunk->data.data())) {
    return nonstd::make_unexpected("Failed to load chunk from callback");
  }

  chunk->is_loaded = true;

  // Update statistics
  stats_.chunks_loaded++;

  return chunk;
}

nonstd::expected<std::shared_ptr<ChunkReader::Chunk>, std::string>
ChunkReader::GetChunk(size_t chunk_id) {
  std::unique_lock<Mutex> lock(cache_mutex_);

  // Detect access pattern
  DetectAccessPattern(chunk_id);

  // Try to find chunk in different caches
  std::shared_ptr<Chunk> chunk;

  // Check sliding window first (most likely for sequential access)
  chunk = FindInSlidingWindow(chunk_id);
  if (chunk) {
    stats_.cache_hits++;
    //return chunk;
  }

  if (!chunk) {
    // Check random access cache
    chunk = FindInRandomCache(chunk_id);
    if (chunk) {
      stats_.cache_hits++;
      //return chunk;
    }
  }

  if (!chunk) {
    // Check preload cache
    chunk = FindInPreloadCache(chunk_id);
    if (chunk) {
      stats_.cache_hits++;
      // Move from preload to appropriate main cache
      preload_cache_.erase(chunk_id);
      current_preload_size_ -= chunk->size;
      InsertIntoCache(chunk_id, chunk);
      //return chunk;
    }
  }

  if (!chunk) {
    // Cache miss
    stats_.cache_misses++;

    // Need to load the chunk
    // First, release the lock to avoid blocking during I/O
    lock.unlock();
    auto chunk_result = LoadChunk(chunk_id);
    lock.lock();

    if (chunk_result) {
      // Another thread may have loaded and inserted this chunk while we
      // were doing I/O; prefer the cached copy to avoid a double-insert.
      if (auto existing_sw = FindInSlidingWindow(chunk_id)) {
        chunk = existing_sw;
      } else if (auto existing_rc = FindInRandomCache(chunk_id)) {
        chunk = existing_rc;
      } else if (auto existing_pc = FindInPreloadCache(chunk_id)) {
        preload_cache_.erase(chunk_id);
        current_preload_size_ -= existing_pc->size;
        chunk = existing_pc;
        InsertIntoCache(chunk_id, chunk);
      } else {
        chunk = chunk_result.value();
        InsertIntoCache(chunk_id, chunk);
      }
    }
  }

  return chunk;
}


nonstd::expected<size_t, std::string> ChunkReader::LoadChunks(const std::vector<size_t>& chunk_ids) {
  size_t loaded_count = 0;

  for (size_t chunk_id : chunk_ids) {
    // Check if already cached
    if (IsChunkCached(chunk_id)) {
      continue; // Already cached
    }

    // Load the chunk
    auto result = GetChunk(chunk_id);
    if (result) {
      loaded_count++;
    } else {
      return nonstd::make_unexpected("Failed to load chunk " + std::to_string(chunk_id) + ": " + result.error());
    }
  }

  return loaded_count;
}

void ChunkReader::InitializeCacheSizes() {
  // Calculate cache sizes based on ratios
  size_t total_chunk_capacity = config_.max_buffer_size / config_.chunk_size;

  sliding_window_size_ = static_cast<size_t>((double(total_chunk_capacity) * double(config_.sliding_window_ratio)) / 100.0);
  random_cache_size_ = static_cast<size_t>((double(total_chunk_capacity) * double(config_.random_cache_ratio)) / 100.0);
  preload_size_ = static_cast<size_t>((double(total_chunk_capacity) * double(config_.preload_ratio)) / 100.0);

  // Ensure at least 1 chunk for each cache if total capacity allows
  if (sliding_window_size_ == 0 && total_chunk_capacity > 0) sliding_window_size_ = 1;
  if (random_cache_size_ == 0 && total_chunk_capacity > 1) random_cache_size_ = 1;
  if (preload_size_ == 0 && total_chunk_capacity > 2) preload_size_ = 1;

  // Adjust if total exceeds capacity
  size_t total_allocated = sliding_window_size_ + random_cache_size_ + preload_size_;
  if (total_allocated > total_chunk_capacity) {
    // Scale down proportionally
    sliding_window_size_ = (sliding_window_size_ * total_chunk_capacity) / total_allocated;
    random_cache_size_ = (random_cache_size_ * total_chunk_capacity) / total_allocated;
    preload_size_ = total_chunk_capacity - sliding_window_size_ - random_cache_size_;
  }

  // Initialize sliding window ring buffer
  if (sliding_window_size_ > 0) {
    sliding_window_ = std::make_unique<RingBuffer>(sliding_window_size_);
  }

  // Initialize 2Q cache structures
  if (random_cache_size_ > 0) {
    if (config_.cache_algorithm == Config::ALGORITHM_2Q) {
      two_q_cache_ = std::make_unique<TwoQCache>(random_cache_size_);
    } else if (config_.cache_algorithm == Config::ALGORITHM_TINYLFU) {
      tiny_lfu_sketch_ = std::make_unique<TinyLFUSketch>(random_cache_size_ * 4);
    }
  }
}

void ChunkReader::DetectAccessPattern(size_t chunk_id) {
  if (last_accessed_chunk_ != SIZE_MAX) {
    if (chunk_id == last_accessed_chunk_ + 1) {
      sequential_access_count_++;
      if (sequential_access_count_ >= 3) {
        in_sequential_mode_ = true;
      }
    } else {
      sequential_access_count_ = 0;
      if (sequential_access_count_ == 0) {
        in_sequential_mode_ = false;
      }
    }
  }
  last_accessed_chunk_ = chunk_id;
}

std::shared_ptr<ChunkReader::Chunk> ChunkReader::FindInSlidingWindow(size_t chunk_id) {
  if (!sliding_window_ || !sliding_window_->contains(chunk_id)) {
    return nullptr;
  }

  size_t index = sliding_window_->get_index(chunk_id);
  return sliding_window_->chunks[index];
}

std::shared_ptr<ChunkReader::Chunk> ChunkReader::FindInRandomCache(size_t chunk_id) {
  auto it = random_cache_.find(chunk_id);
  if (it == random_cache_.end()) {
    return nullptr;
  }

  // Update access information based on algorithm
  CacheEntry& entry = it->second;
  entry.access_time = ++global_timestamp_;
  entry.access_count++;

  switch (config_.cache_algorithm) {
    case Config::ALGORITHM_2Q:
      Update2Q(chunk_id, entry);
      break;
    case Config::ALGORITHM_TINYLFU:
      UpdateTinyLFU(chunk_id, entry);
      break;
    case Config::ALGORITHM_SLRU:
      // Move to front of LRU list
      random_cache_lru_.erase(entry.list_iterator);
      random_cache_lru_.push_front(chunk_id);
      entry.list_iterator = random_cache_lru_.begin();
      break;
  }

  return entry.chunk;
}

std::shared_ptr<ChunkReader::Chunk> ChunkReader::FindInPreloadCache(size_t chunk_id) {
  auto it = preload_cache_.find(chunk_id);
  return (it != preload_cache_.end()) ? it->second : nullptr;
}

void ChunkReader::InsertIntoCache(size_t chunk_id, std::shared_ptr<Chunk> chunk) {
  // Choose cache based on access pattern and availability
  if (in_sequential_mode_ && sliding_window_) {
    InsertIntoSlidingWindow(chunk_id, chunk);
  } else if (random_cache_size_ > 0) {
    InsertIntoRandomCache(chunk_id, chunk);
  } else if (sliding_window_) {
    InsertIntoSlidingWindow(chunk_id, chunk);
  }
}

void ChunkReader::InsertIntoSlidingWindow(size_t chunk_id, std::shared_ptr<Chunk> chunk) {
  if (!sliding_window_) return;

  // Evict if necessary
  while (current_sliding_window_size_ + chunk->size > sliding_window_size_ * config_.chunk_size) {
    EvictFromSlidingWindow();
  }

  // Handle sequential insertion
  if (sliding_window_->is_empty()) {
    sliding_window_->base_chunk_id = chunk_id;
    sliding_window_->tail = 0;
    sliding_window_->head = 0;
    sliding_window_->size = 1;
    sliding_window_->chunks[0] = chunk;
  } else {
    // Check if this extends the window
    if (chunk_id == sliding_window_->base_chunk_id + sliding_window_->size) {
      // Extend window forward
      if (sliding_window_->is_full()) {
        // Advance tail and update base_chunk_id
        sliding_window_->chunks[sliding_window_->tail] = nullptr;
        sliding_window_->tail = (sliding_window_->tail + 1) % sliding_window_->capacity;
        sliding_window_->base_chunk_id++;
        sliding_window_->size--;
        current_sliding_window_size_ -= config_.chunk_size;
      }

      sliding_window_->head = (sliding_window_->head + 1) % sliding_window_->capacity;
      sliding_window_->chunks[sliding_window_->head] = chunk;
      sliding_window_->size++;
    } else {
      // Non-sequential access, clear and restart window
      sliding_window_->chunks.assign(sliding_window_->capacity, nullptr);
      sliding_window_->base_chunk_id = chunk_id;
      sliding_window_->head = 0;
      sliding_window_->tail = 0;
      sliding_window_->size = 1;
      sliding_window_->chunks[0] = chunk;
      current_sliding_window_size_ = chunk->size;
    }
  }

  current_sliding_window_size_ += chunk->size;
}

void ChunkReader::InsertIntoRandomCache(size_t chunk_id, std::shared_ptr<Chunk> chunk) {
  // Evict if necessary
  while (current_random_cache_size_ + chunk->size > random_cache_size_ * config_.chunk_size) {
    EvictFromRandomCache();
  }

  CacheEntry entry;
  entry.chunk = chunk;
  entry.cache_type = CACHE_RANDOM_ACCESS;
  entry.access_time = ++global_timestamp_;
  entry.access_count = 1;

  switch (config_.cache_algorithm) {
    case Config::ALGORITHM_2Q:
      entry.in_a1 = true;
      two_q_cache_->a1_fifo.push_back(chunk_id);
      entry.list_iterator = --two_q_cache_->a1_fifo.end();
      break;

    case Config::ALGORITHM_TINYLFU:
      entry.frequency = 1;
      tiny_lfu_sketch_->increment(chunk_id);
      random_cache_lru_.push_front(chunk_id);
      entry.list_iterator = random_cache_lru_.begin();
      break;

    case Config::ALGORITHM_SLRU:
      random_cache_lru_.push_front(chunk_id);
      entry.list_iterator = random_cache_lru_.begin();
      break;
  }

  random_cache_[chunk_id] = entry;
  current_random_cache_size_ += chunk->size;
}

void ChunkReader::InsertIntoPreloadCache(size_t chunk_id, std::shared_ptr<Chunk> chunk) {
  // Evict if necessary
  while (current_preload_size_ + chunk->size > preload_size_ * config_.chunk_size) {
    EvictFromPreloadCache();
  }

  preload_cache_[chunk_id] = chunk;
  preload_lru_.push_front(chunk_id);
  current_preload_size_ += chunk->size;
}

void ChunkReader::EvictFromSlidingWindow() {
  if (!sliding_window_ || sliding_window_->is_empty()) return;

  // Remove from tail
  auto chunk = sliding_window_->chunks[sliding_window_->tail];
  sliding_window_->chunks[sliding_window_->tail] = nullptr;
  sliding_window_->tail = (sliding_window_->tail + 1) % sliding_window_->capacity;
  sliding_window_->base_chunk_id++;
  sliding_window_->size--;

  if (chunk) {
    current_sliding_window_size_ -= chunk->size;
    stats_.chunks_evicted++;
  }
}

void ChunkReader::EvictFromRandomCache() {
  if (random_cache_.empty()) return;

  size_t victim_chunk_id = 0;

  switch (config_.cache_algorithm) {
    case Config::ALGORITHM_2Q: {
      // Evict from A1 queue first (FIFO), then from Am queue (LRU)
      if (!two_q_cache_->a1_fifo.empty()) {
        victim_chunk_id = two_q_cache_->a1_fifo.front();
        two_q_cache_->a1_fifo.pop_front();
      } else if (!two_q_cache_->am_lru.empty()) {
        victim_chunk_id = two_q_cache_->am_lru.back();
        two_q_cache_->am_lru.pop_back();
      } else {
        return;
      }
      break;
    }

    case Config::ALGORITHM_TINYLFU: {
      // Evict least frequently used item
      uint8_t min_freq = 255;
      auto victim_it = random_cache_.end();

      for (auto it = random_cache_.begin(); it != random_cache_.end(); ++it) {
        uint8_t freq = tiny_lfu_sketch_->estimate(it->first);
        if (freq < min_freq) {
          min_freq = freq;
          victim_it = it;
        }
      }

      if (victim_it != random_cache_.end()) {
        victim_chunk_id = victim_it->first;
        random_cache_lru_.erase(victim_it->second.list_iterator);
      } else {
        return;
      }
      break;
    }

    case Config::ALGORITHM_SLRU: {
      // Simple LRU eviction
      if (!random_cache_lru_.empty()) {
        victim_chunk_id = random_cache_lru_.back();
        random_cache_lru_.pop_back();
      } else {
        return;
      }
      break;
    }
  }

  auto it = random_cache_.find(victim_chunk_id);
  if (it != random_cache_.end()) {
    current_random_cache_size_ -= it->second.chunk->size;
    random_cache_.erase(it);
    stats_.chunks_evicted++;
  }
}

void ChunkReader::EvictFromPreloadCache() {
  if (preload_lru_.empty()) return;

  size_t victim_chunk_id = preload_lru_.back();
  preload_lru_.pop_back();

  auto it = preload_cache_.find(victim_chunk_id);
  if (it != preload_cache_.end()) {
    current_preload_size_ -= it->second->size;
    preload_cache_.erase(it);
    stats_.chunks_evicted++;
  }
}

void ChunkReader::Update2Q(size_t chunk_id, CacheEntry& entry) {
  if (entry.in_a1) {
    // Move from A1 to Am
    two_q_cache_->a1_fifo.erase(entry.list_iterator);

    // Evict from Am if necessary
    while (two_q_cache_->am_lru.size() >= two_q_cache_->am_max_size) {
      size_t evict_id = two_q_cache_->am_lru.back();
      two_q_cache_->am_lru.pop_back();
      auto evict_it = random_cache_.find(evict_id);
      if (evict_it != random_cache_.end()) {
        current_random_cache_size_ -= evict_it->second.chunk->size;
        random_cache_.erase(evict_it);
      }
    }

    two_q_cache_->am_lru.push_front(chunk_id);
    entry.list_iterator = two_q_cache_->am_lru.begin();
    entry.in_a1 = false;
  } else {
    // Already in Am, move to front
    two_q_cache_->am_lru.erase(entry.list_iterator);
    two_q_cache_->am_lru.push_front(chunk_id);
    entry.list_iterator = two_q_cache_->am_lru.begin();
  }
}

void ChunkReader::UpdateTinyLFU(size_t chunk_id, CacheEntry& entry) {
  tiny_lfu_sketch_->increment(chunk_id);
  entry.frequency = tiny_lfu_sketch_->estimate(chunk_id);

  // Move to front of LRU list
  random_cache_lru_.erase(entry.list_iterator);
  random_cache_lru_.push_front(chunk_id);
  entry.list_iterator = random_cache_lru_.begin();
}

bool ChunkReader::IsAnyCacheFull() const {
  return (sliding_window_ && sliding_window_->is_full()) ||
         (current_random_cache_size_ >= random_cache_size_ * config_.chunk_size) ||
         (current_preload_size_ >= preload_size_ * config_.chunk_size);
}

} // namespace lightusd
