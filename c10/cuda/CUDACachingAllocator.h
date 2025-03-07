#ifndef THC_DEVICE_ALLOCATOR_INC
#define THC_DEVICE_ALLOCATOR_INC
#include <c10/core/Allocator.h>
#include <c10/cuda/CUDAGraphsC10Utils.h>
#include <c10/cuda/CUDAMacros.h>
#include <c10/cuda/CUDAStream.h>
#include <c10/util/Registry.h>

#include <array>
#include <mutex>

namespace c10 {

// Caching allocator will execute every registered callback if it unable to find
// block inside of already allocated area.
class C10_CUDA_API FreeMemoryCallback {
 public:
  virtual ~FreeMemoryCallback() = default;
  virtual bool Execute() = 0;
};

C10_DECLARE_REGISTRY(FreeCudaMemoryCallbacksRegistry, FreeMemoryCallback);
#define REGISTER_FREE_MEMORY_CALLBACK(name, ...) \
  C10_REGISTER_CLASS(FreeCudaMemoryCallbacksRegistry, name, __VA_ARGS__);

namespace cuda {

// TODO: Turn this into an honest to goodness class. I briefly attempted to do
// this, but it was a bit irritating to figure out how to also correctly
// apply pimpl pattern so I didn't have to leak any internal implementation
// details in the header (CUDACachingAllocator could be made a pimpl, but
// you also need to appropriately define a class which is a subclass
// of Allocator. Not impossible, but required a bit more surgery than
// I wanted to do at the time.)
//
// Why is this using a namespace rather than old-style THCCachingAllocator_
// prefix?  Mostly because it made the HIPify rules easier to write; _ is
// not counted as a word boundary, so you would otherwise have to list each
// of these functions.

namespace CUDACachingAllocator {

struct Stat {
  int64_t current = 0;
  int64_t peak = 0;
  int64_t allocated = 0;
  int64_t freed = 0;
};

enum struct StatType : uint64_t {
  AGGREGATE = 0,
  SMALL_POOL = 1,
  LARGE_POOL = 2,
  NUM_TYPES = 3 // remember to update this whenever a new stat type is added
};

typedef std::array<Stat, static_cast<size_t>(StatType::NUM_TYPES)> StatArray;

// Struct containing memory allocator summary statistics for a device.
struct DeviceStats {
  // COUNT: allocations requested by client code
  StatArray allocation;
  // COUNT: number of allocated segments from cudaMalloc().
  StatArray segment;
  // COUNT: number of active memory blocks (allocated or used by stream)
  StatArray active;
  // COUNT: number of inactive, split memory blocks (unallocated but can't be
  // released via cudaFree)
  StatArray inactive_split;

  // SUM: bytes requested by client code
  StatArray allocated_bytes;
  // SUM: bytes reserved by this memory allocator (both free and used)
  StatArray reserved_bytes;
  // SUM: bytes within active memory blocks
  StatArray active_bytes;
  // SUM: bytes within inactive, split memory blocks
  StatArray inactive_split_bytes;

  // COUNT: total number of failed calls to CUDA malloc necessitating cache
  // flushes.
  int64_t num_alloc_retries = 0;

  // COUNT: total number of OOMs (i.e. failed calls to CUDA after cache flush)
  int64_t num_ooms = 0;

  // COUNT: total number of oversize blocks allocated from pool
  Stat oversize_allocations;

  // COUNT: total number of oversize blocks requiring malloc
  Stat oversize_segments;

  // SIZE: maximum block size that is allowed to be split.
  int64_t max_split_size = 0;
};

struct Context {
  virtual ~Context() {}
};

typedef std::shared_ptr<Context> (*CreateContextFn)(void);

struct History {
  void* addr;
  size_t real_size; // unrounded, actually requested size
  std::shared_ptr<Context> context; // per-watcher context
};

// Struct containing info of an allocation block (i.e. a fractional part of a
// cudaMalloc)..
struct BlockInfo {
  int64_t size = 0;
  int32_t gc_counter = 0;
  bool allocated = false;
  bool active = false;
  std::vector<History> history;
};

// Struct containing info of a memory segment (i.e. one contiguous cudaMalloc).
struct SegmentInfo {
  int64_t device = 0;
  int64_t address = 0;
  int64_t total_size = 0;
  int64_t allocated_size = 0;
  int64_t active_size = 0;
  cudaStream_t stream = 0;
  bool is_large = false;
  std::vector<BlockInfo> blocks;
};

struct TraceEntry {
  enum Action {
    ALLOC, // API made to the caching allocator for new memory
    FREE_REQUESTED, // API call made to the caching allocator to free memory
    FREE_COMPLETED, // The allocator might have to delay a free because
                    // it is still in use on another stream via record_stream
                    // This event is generated when a free actually completes.
    SEGMENT_ALLOC, // a call to cudaMalloc to get more memory from the OS
    SEGMENT_FREE, // a call to cudaFree to return memory to the OS (e.g. to
                  // defragement or empty_caches)
    SNAPSHOT, // a call to snapshot, used to correlate memory snapshots to trace
              // events
    OOM // the allocator threw an OutOfMemoryError (addr_ is the amount of free
        // bytes reported by cuda)
  };
  TraceEntry(
      Action action,
      int64_t addr,
      size_t size,
      cudaStream_t stream,
      std::shared_ptr<Context> context = nullptr)
      : action_(action),
        addr_(addr),
        context_(context),
        stream_(stream),
        size_(size) {}
  Action action_;
  int64_t addr_; // for OOM, this is the amount of free bytes reported by cuda
  std::shared_ptr<Context> context_;
  cudaStream_t stream_;
  int64_t size_;
};

struct SnapshotInfo {
  std::vector<SegmentInfo> segments;
  std::vector<std::vector<TraceEntry>> device_traces;
};

C10_CUDA_API void* raw_alloc(size_t nbytes);
C10_CUDA_API void* raw_alloc_with_stream(size_t nbytes, cudaStream_t stream);
C10_CUDA_API void raw_delete(void* ptr);

C10_CUDA_API Allocator* get();
C10_CUDA_API void init(int device_count);
C10_CUDA_API void setMemoryFraction(double fraction, int device);
C10_CUDA_API void setAllocatorSettings(const std::string& env);
C10_CUDA_API void emptyCache();
C10_CUDA_API void cacheInfo(
    int dev_id,
    size_t* cachedAndFree,
    size_t* largestBlock);
C10_CUDA_API void* getBaseAllocation(void* ptr, size_t* size);
C10_CUDA_API void recordStream(const DataPtr&, CUDAStream stream);
C10_CUDA_API DeviceStats getDeviceStats(int device);
C10_CUDA_API void resetAccumulatedStats(int device);
C10_CUDA_API void resetPeakStats(int device);
C10_CUDA_API SnapshotInfo snapshot();

// CUDAGraph interactions
C10_CUDA_API void notifyCaptureBegin(
    int device,
    CaptureId_t graph_id,
    MempoolId_t mempool_id);
C10_CUDA_API void notifyCaptureEnd(int device, CaptureId_t graph_id);
C10_CUDA_API void notifyCaptureDestroy(int device, MempoolId_t mempool_id);

C10_CUDA_API std::mutex* getFreeMutex();

C10_CUDA_API void recordHistory(
    bool enabled,
    CreateContextFn context_recorder,
    size_t alloc_trace_max_entries,
    bool alloc_trace_record_context);
using OutOfMemoryObserver = std::function<void(
    int64_t device,
    int64_t allocated,
    int64_t device_total,
    int64_t device_free)>;
C10_CUDA_API void attachOutOfMemoryObserver(OutOfMemoryObserver observer);

C10_CUDA_API std::shared_ptr<void> getIpcDevPtr(std::string handle);
} // namespace CUDACachingAllocator

} // namespace cuda
} // namespace c10

#endif
