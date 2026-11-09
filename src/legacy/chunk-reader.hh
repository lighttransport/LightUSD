// SPDX-License-Identifier: Apache 2.0
// Copyright 2025 - Present, Light Transport Entertainment Inc.
//
// Chunked data reader for streaming USD data parsing with LRU buffer management
#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>

#include "tsa-mutex.hh"
#include <string>
#include <vector>

#include "nonstd/expected.hpp"
#include "tiny-hashmap.hh"

namespace lightusd {

///
/// Chunked data reader for efficient streaming and parsing of large USD files.
/// This reader uses a hybrid caching strategy combining sliding window, 2Q/SLRU/TinyLFU,
/// and preload mechanisms for optimal performance across different access patterns.
/// When a requested chunk is not in cache, it triggers a callback to load the data.
///
/// Key features:
/// - Fixed-size chunks for predictable memory usage
/// - Hybrid caching: Sliding Window (70%) + 2Q/SLRU/TinyLFU (25%) + Preload (5%)
/// - Configurable allocation ratios
/// - Asynchronous chunk loading via callbacks
/// - Thread-safe operations
/// - Optimized for both sequential and random access patterns
///
class ChunkReader {
public:
  ///
  /// Chunk data structure
  ///
  struct Chunk {
    size_t chunk_id;                      ///< Unique identifier for the chunk
    size_t offset;                        ///< Offset in the original data stream
    size_t size;                          ///< Actual data size (may be less than chunk_size for last chunk)
    std::vector<uint8_t> data;           ///< Chunk data buffer
    bool is_loaded;                      ///< Whether the chunk is currently loaded

    Chunk() : chunk_id(0), offset(0), size(0), is_loaded(false) {}
    Chunk(size_t id, size_t off, size_t sz)
      : chunk_id(id), offset(off), size(sz), data(sz), is_loaded(false) {}
  };

  ///
  /// Callback function type for chunk requests.
  /// Called when a chunk needs to be loaded that is not in the cache.
  ///
  /// @param[in] chunk_id The ID of the requested chunk
  /// @param[in] offset The byte offset in the stream
  /// @param[in] size The size of data to read
  /// @param[out] data Buffer to fill with chunk data
  /// @return true if chunk was successfully loaded, false otherwise
  ///
  using ChunkRequestCallback = std::function<bool(size_t chunk_id, size_t offset, size_t size, uint8_t* data)>;

  ///
  /// Configuration for the chunk reader
  ///
  struct Config {
    size_t chunk_size;        ///< Size of each chunk in bytes (default: 1MB)
    size_t max_chunks;        ///< Maximum number of chunks to keep in memory
    size_t max_buffer_size;   ///< Maximum total buffer size in bytes (default: 16MB)
    bool enable_prefetch;     ///< Enable prefetching of adjacent chunks
    size_t prefetch_distance; ///< Number of chunks to prefetch ahead

    // Allocation ratios for hybrid caching (should sum to 100)
    float sliding_window_ratio;  ///< Percentage for sliding window cache (default: 70%)
    float random_cache_ratio;    ///< Percentage for 2Q/SLRU/TinyLFU cache (default: 25%)
    float preload_ratio;         ///< Percentage for preload buffer (default: 5%)

    // Cache algorithm selection
    enum CacheAlgorithm {
      ALGORITHM_2Q,      ///< 2Q algorithm for random access
      ALGORITHM_SLRU,    ///< Segmented LRU
      ALGORITHM_TINYLFU  ///< TinyLFU with W-TinyLFU
    };
    CacheAlgorithm cache_algorithm; ///< Algorithm to use for random access cache

    Config()
      : chunk_size(1024 * 1024)
      , max_chunks(16)
      , max_buffer_size(16 * 1024 * 1024)
      , enable_prefetch(true)
      , prefetch_distance(2)
      , sliding_window_ratio(70.0f)
      , random_cache_ratio(25.0f)
      , preload_ratio(5.0f)
      , cache_algorithm(ALGORITHM_2Q) {}
  };

  ///
  /// Statistics for monitoring chunk reader performance
  ///
  struct Stats {
    size_t total_reads;          ///< Total number of read operations
    size_t cache_hits;           ///< Number of cache hits
    size_t cache_misses;         ///< Number of cache misses
    size_t chunks_loaded;        ///< Total chunks loaded
    size_t chunks_evicted;       ///< Total chunks evicted from cache
    size_t bytes_read;           ///< Total bytes read

    Stats()
      : total_reads(0)
      , cache_hits(0)
      , cache_misses(0)
      , chunks_loaded(0)
      , chunks_evicted(0)
      , bytes_read(0) {}

    double hit_rate() const {
      if (total_reads == 0) return 0.0;
      return static_cast<double>(cache_hits) / double(total_reads);
    }
  };

  ///
  /// Constructor
  ///
  /// @param[in] total_size Total size of the data stream in bytes
  /// @param[in] config Configuration parameters
  ///
  ChunkReader(size_t total_size, const Config& config = Config());

  ///
  /// Destructor
  ///
  ~ChunkReader();

  ///
  /// Set the chunk request callback
  ///
  /// @param[in] callback Function to call when a chunk needs to be loaded
  ///
  void SetChunkRequestCallback(ChunkRequestCallback callback) {
    chunk_request_callback_ = callback;
  }

  ///
  /// Information about required chunks for a read operation
  ///
  struct ReadResult {
    std::vector<size_t> required_chunks;  ///< List of chunk IDs needed but not in cache
    size_t bytes_available;                ///< Number of bytes that can be read from cache
    bool fully_cached;                     ///< True if all data is in cache
  };

  ///
  /// Read data from the stream
  ///
  /// @param[in] offset Byte offset to start reading from
  /// @param[in] size Number of bytes to read
  /// @param[out] buffer Buffer to store the read data (optional, can be null to just check)
  /// @return Expected containing ReadResult with required chunks info, or error string
  ///
  /// Error conditions:
  /// - Read exceeds EOF (total_size)
  /// - Read size exceeds chunk_size * max_chunks
  /// - Invalid offset
  ///
  /// If buffer is null, only returns information about required chunks without reading.
  /// If buffer is provided and all data is cached, performs the read.
  /// If buffer is provided but data is not cached, returns required chunks list.
  ///
  nonstd::expected<ReadResult, std::string> Read(size_t offset, size_t size, uint8_t* buffer = nullptr) LIGHTUSD_REQUIRES_NOT(cache_mutex_);

  ///
  /// Read data from the stream (legacy interface for compatibility)
  ///
  /// @param[in] offset Byte offset to start reading from
  /// @param[in] size Number of bytes to read
  /// @param[out] buffer Buffer to store the read data
  /// @param[in] force_load If true, loads required chunks via callback
  /// @return Expected containing number of bytes read, or error string
  ///
  nonstd::expected<size_t, std::string> ReadDirect(size_t offset, size_t size, uint8_t* buffer, bool force_load = true) LIGHTUSD_REQUIRES_NOT(cache_mutex_);

  ///
  /// Read a single byte
  ///
  /// @param[in] offset Byte offset to read from
  /// @param[out] value The byte value
  /// @return Expected containing true on success, or error string
  ///
  nonstd::expected<bool, std::string> ReadByte(size_t offset, uint8_t* value) LIGHTUSD_REQUIRES_NOT(cache_mutex_);

  ///
  /// Read 2 bytes (16-bit value)
  ///
  /// @param[in] offset Byte offset to read from
  /// @param[out] value The 16-bit value
  /// @return Expected containing true on success, or error string
  ///
  nonstd::expected<bool, std::string> Read2(size_t offset, uint16_t* value) LIGHTUSD_REQUIRES_NOT(cache_mutex_);

  ///
  /// Read 4 bytes (32-bit value)
  ///
  /// @param[in] offset Byte offset to read from
  /// @param[out] value The 32-bit value
  /// @return Expected containing true on success, or error string
  ///
  nonstd::expected<bool, std::string> Read4(size_t offset, uint32_t* value) LIGHTUSD_REQUIRES_NOT(cache_mutex_);

  ///
  /// Read 8 bytes (64-bit value)
  ///
  /// @param[in] offset Byte offset to read from
  /// @param[out] value The 64-bit value
  /// @return Expected containing true on success, or error string
  ///
  nonstd::expected<bool, std::string> Read8(size_t offset, uint64_t* value) LIGHTUSD_REQUIRES_NOT(cache_mutex_);

  ///
  /// Seek to a position in the stream
  ///
  /// @param[in] offset Byte offset to seek to
  /// @return true if seek was successful
  ///
  bool Seek(size_t offset);

  ///
  /// Get current position in the stream
  ///
  /// @return Current byte offset
  ///
  size_t Tell() const { return current_offset_; }

  ///
  /// Get total size of the stream
  ///
  /// @return Total size in bytes
  ///
  size_t Size() const { return total_size_; }

  ///
  /// Prefetch chunks starting from the given offset
  ///
  /// @param[in] offset Starting byte offset
  /// @param[in] size Size of data that will be read
  ///
  void Prefetch(size_t offset, size_t size);

  ///
  /// Clear all cached chunks
  ///
  void ClearCache() LIGHTUSD_REQUIRES_NOT(cache_mutex_);

  ///
  /// Get statistics about cache performance
  ///
  /// @return Current statistics
  ///
  Stats GetStats() const;

  ///
  /// Reset statistics
  ///
  void ResetStats();

  ///
  /// Check if a chunk is currently in cache
  ///
  /// @param[in] chunk_id The chunk ID to check
  /// @return true if chunk is in cache
  ///
  bool IsChunkCached(size_t chunk_id) const;

  ///
  /// Get the current cache size in bytes
  ///
  /// @return Current cache size
  ///
  size_t GetCacheSize() const LIGHTUSD_REQUIRES_NOT(cache_mutex_);

  ///
  /// Get the number of chunks currently in cache
  ///
  /// @return Number of cached chunks
  ///
  size_t GetCachedChunkCount() const LIGHTUSD_REQUIRES_NOT(cache_mutex_);

  ///
  /// Load specific chunks into cache
  ///
  /// @param[in] chunk_ids List of chunk IDs to load
  /// @return Expected containing number of chunks loaded, or error string
  ///
  nonstd::expected<size_t, std::string> LoadChunks(const std::vector<size_t>& chunk_ids);

  ///
  /// Get maximum readable size in one operation
  ///
  /// @return Maximum bytes that can be read in one operation
  ///
  size_t GetMaxReadSize() const {
    return config_.chunk_size * config_.max_chunks;
  }

private:
  ///
  /// Cache type enumeration
  ///
  enum CacheType {
    CACHE_SLIDING_WINDOW,
    CACHE_RANDOM_ACCESS,
    CACHE_PRELOAD
  };

  ///
  /// Internal chunk cache entry for hybrid management
  ///
  struct CacheEntry {
    std::shared_ptr<Chunk> chunk;
    CacheType cache_type;
    uint64_t access_time;
    uint32_t access_count;

    // For 2Q algorithm
    bool in_a1;  ///< In A1 queue (first-time access)

    // For TinyLFU
    uint32_t frequency;

    // For linked list management
    std::list<size_t>::iterator list_iterator;

    CacheEntry() : cache_type(CACHE_SLIDING_WINDOW), access_time(0),
                   access_count(0), in_a1(true), frequency(0) {}
  };

  ///
  /// Ring buffer for sliding window cache
  ///
  struct RingBuffer {
    std::vector<std::shared_ptr<Chunk>> chunks;
    size_t head;         ///< Current write position
    size_t tail;         ///< Current read position
    size_t capacity;     ///< Maximum number of chunks
    size_t size;         ///< Current number of chunks
    size_t base_chunk_id; ///< ID of chunk at tail position

    RingBuffer(size_t cap) : head(0), tail(0), capacity(cap), size(0), base_chunk_id(0) {
      chunks.resize(cap);
    }

    bool is_empty() const { return size == 0; }
    bool is_full() const { return size == capacity; }
    bool contains(size_t chunk_id) const {
      return !is_empty() && chunk_id >= base_chunk_id &&
             chunk_id < base_chunk_id + size;
    }
    size_t get_index(size_t chunk_id) const {
      return (tail + (chunk_id - base_chunk_id)) % capacity;
    }
  };

  ///
  /// 2Q cache implementation
  ///
  struct TwoQCache {
    std::list<size_t> a1_fifo;    ///< A1 queue (FIFO for first access)
    std::list<size_t> am_lru;     ///< Am queue (LRU for frequent access)
    size_t a1_max_size;
    size_t am_max_size;

    TwoQCache(size_t total_size) {
      // Standard 2Q ratios: A1=25%, Am=75%
      a1_max_size = total_size / 4;
      am_max_size = total_size - a1_max_size;
      if (a1_max_size == 0) a1_max_size = 1;
      if (am_max_size == 0) am_max_size = 1;
    }
  };

  ///
  /// TinyLFU sketch for frequency estimation
  ///
  struct TinyLFUSketch {
    std::vector<uint8_t> sketch;
    size_t size;
    uint64_t total_count;

    TinyLFUSketch(size_t sz) : size(sz), total_count(0) {
      sketch.resize(sz, 0);
    }

    void increment(size_t item) {
      size_t hash_val = std::hash<size_t>{}(item);
      size_t index = hash_val % size;
      if (sketch[index] < 255) {
        sketch[index]++;
      }
      total_count++;

      // Reset periodically to handle aging
      if (total_count > size * 10) {
        for (auto& val : sketch) {
          val /= 2;
        }
        total_count /= 2;
      }
    }

    uint8_t estimate(size_t item) const {
      size_t hash_val = std::hash<size_t>{}(item);
      size_t index = hash_val % size;
      return sketch[index];
    }
  };

  ///
  /// Calculate chunk ID from byte offset
  ///
  size_t GetChunkId(size_t offset) const {
    return offset / config_.chunk_size;
  }

  ///
  /// Calculate offset within a chunk
  ///
  size_t GetChunkOffset(size_t offset) const {
    return offset % config_.chunk_size;
  }

  ///
  /// Load a chunk into cache
  ///
  /// @param[in] chunk_id The chunk ID to load
  /// @return Expected containing the loaded chunk, or error string
  ///
  nonstd::expected<std::shared_ptr<Chunk>, std::string> LoadChunk(size_t chunk_id);

  ///
  /// Get a chunk from cache or load it
  ///
  /// @param[in] chunk_id The chunk ID to get
  /// @return Expected containing the chunk, or error string
  ///
  nonstd::expected<std::shared_ptr<Chunk>, std::string> GetChunk(size_t chunk_id);

  ///
  /// Detect access pattern (sequential vs random)
  ///
  /// @param[in] chunk_id The chunk ID being accessed
  ///
  void DetectAccessPattern(size_t chunk_id);

  ///
  /// Try to find chunk in sliding window cache
  ///
  /// @param[in] chunk_id The chunk ID to find
  /// @return Shared pointer to chunk if found, nullptr otherwise
  ///
  std::shared_ptr<Chunk> FindInSlidingWindow(size_t chunk_id);

  ///
  /// Try to find chunk in random access cache
  ///
  /// @param[in] chunk_id The chunk ID to find
  /// @return Shared pointer to chunk if found, nullptr otherwise
  ///
  std::shared_ptr<Chunk> FindInRandomCache(size_t chunk_id);

  ///
  /// Try to find chunk in preload cache
  ///
  /// @param[in] chunk_id The chunk ID to find
  /// @return Shared pointer to chunk if found, nullptr otherwise
  ///
  std::shared_ptr<Chunk> FindInPreloadCache(size_t chunk_id);

  ///
  /// Insert chunk into appropriate cache based on access pattern
  ///
  /// @param[in] chunk_id The chunk ID
  /// @param[in] chunk The chunk to insert
  ///
  void InsertIntoCache(size_t chunk_id, std::shared_ptr<Chunk> chunk);

  ///
  /// Insert chunk into sliding window cache
  ///
  /// @param[in] chunk_id The chunk ID
  /// @param[in] chunk The chunk to insert
  ///
  void InsertIntoSlidingWindow(size_t chunk_id, std::shared_ptr<Chunk> chunk);

  ///
  /// Insert chunk into random access cache
  ///
  /// @param[in] chunk_id The chunk ID
  /// @param[in] chunk The chunk to insert
  ///
  void InsertIntoRandomCache(size_t chunk_id, std::shared_ptr<Chunk> chunk);

  ///
  /// Insert chunk into preload cache
  ///
  /// @param[in] chunk_id The chunk ID
  /// @param[in] chunk The chunk to insert
  ///
  void InsertIntoPreloadCache(size_t chunk_id, std::shared_ptr<Chunk> chunk);

  ///
  /// Evict chunks from sliding window cache
  ///
  void EvictFromSlidingWindow();

  ///
  /// Evict chunks from random access cache using selected algorithm
  ///
  void EvictFromRandomCache();

  ///
  /// Evict chunks from preload cache
  ///
  void EvictFromPreloadCache();

  ///
  /// Update access information for 2Q algorithm
  ///
  /// @param[in] chunk_id The chunk ID being accessed
  /// @param[in] entry Cache entry to update
  ///
  void Update2Q(size_t chunk_id, CacheEntry& entry);

  ///
  /// Update access information for TinyLFU algorithm
  ///
  /// @param[in] chunk_id The chunk ID being accessed
  /// @param[in] entry Cache entry to update
  ///
  void UpdateTinyLFU(size_t chunk_id, CacheEntry& entry);

  ///
  /// Check if any cache is full
  ///
  bool IsAnyCacheFull() const;

  ///
  /// Initialize cache sizes based on allocation ratios
  ///
  void InitializeCacheSizes();

  // Configuration
  Config config_;
  size_t total_size_;
  size_t current_offset_;

  // Hybrid cache management
  mutable Mutex cache_mutex_;

  // Cache size allocations
  size_t sliding_window_size_;
  size_t random_cache_size_;
  size_t preload_size_;

  // Sliding window cache (ring buffer)
  std::unique_ptr<RingBuffer> sliding_window_;

  // Random access cache
  lightusd::HashMap<size_t, CacheEntry> random_cache_;
  std::unique_ptr<TwoQCache> two_q_cache_;
  std::unique_ptr<TinyLFUSketch> tiny_lfu_sketch_;
  std::list<size_t> random_cache_lru_;  ///< For SLRU algorithm

  // Preload cache
  lightusd::HashMap<size_t, std::shared_ptr<Chunk>> preload_cache_;
  std::list<size_t> preload_lru_;

  // Current cache sizes
  size_t current_sliding_window_size_;
  size_t current_random_cache_size_;
  size_t current_preload_size_;

  // Access pattern detection
  size_t last_accessed_chunk_;
  size_t sequential_access_count_;
  bool in_sequential_mode_;

  // Global timestamp for access ordering
  uint64_t global_timestamp_;

  // Callback for loading chunks
  ChunkRequestCallback chunk_request_callback_;

  // Statistics
  mutable Stats stats_;

  // Disable copy
  ChunkReader(const ChunkReader&) = delete;
  ChunkReader& operator=(const ChunkReader&) = delete;
};

} // namespace lightusd
