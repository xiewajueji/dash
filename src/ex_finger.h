#ifndef CUCKOO
#define CUCKOO

#include <immintrin.h>
#include <bitset>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <shared_mutex>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "../util/hash.h"
#include "../util/pair.h"
#include "../util/persist.h"
#include "allocator.h"

#ifdef PMEM
#include <libpmemobj.h>
#endif

#define _INVALID 0 /* we use 0 as the invalid key*/
#define SINGLE 1
//#define COUNTING 1

#define SIMD 1
#define SIMD_CMP8(src, key)                                         \
  do {                                                              \
    const __m256i key_data = _mm256_set1_epi8(key);                 \
    __m256i seg_data =                                              \
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src)); \
    __m256i rv_mask = _mm256_cmpeq_epi8(seg_data, key_data);        \
    mask = _mm256_movemask_epi8(rv_mask);                           \
  } while (0)

#define SSE_CMP8(src, key)                                       \
  do {                                                           \
    const __m128i key_data = _mm_set1_epi8(key);                 \
    __m128i seg_data =                                           \
        _mm_loadu_si128(reinterpret_cast<const __m128i *>(src)); \
    __m128i rv_mask = _mm_cmpeq_epi8(seg_data, key_data);        \
    mask = _mm_movemask_epi8(rv_mask);                           \
  } while (0)

#define CHECK_BIT(var, pos) ((((var) & (1 << pos)) > 0) ? (1) : (0))

const uint32_t lockSet = ((uint64_t)1 << 31);      /*locking information*/
const uint32_t lockMask = ((uint64_t)1 << 31) - 1; /*locking mask*/
const int overflowSet = 1 << 15;
const int countMask = (1 << 4) - 1;

struct _Pair {
  Key_t key;
  Value_t value;
};

constexpr size_t k_PairSize = sizeof(_Pair);  // a k-v _Pair with a bit
constexpr size_t kNumPairPerBucket =
    14; /* it is determined by the usage of the fingerprint*/
constexpr size_t kFingerBits = 8;
constexpr size_t kMask = (1 << kFingerBits) - 1;
const constexpr size_t kNumBucket = 64;
constexpr size_t stashBucket = 2;
constexpr int allocMask = (1 << kNumPairPerBucket) - 1;
constexpr size_t bucketMask = ((1 << (int)log2(kNumBucket)) - 1);
constexpr size_t stashMask = (1 << (int)log2(stashBucket)) - 1;
constexpr uint8_t stashHighMask = ~((uint8_t)stashMask);
constexpr uint8_t preNeighborSet = 1 << 7;
constexpr uint8_t nextNeighborSet = 1 << 6;
#define BUCKET_INDEX(hash) ((hash >> kFingerBits) % kNumBucket)
#define GET_COUNT(var) ((var)&countMask)
#define GET_BITMAP(var) (((var) >> 4) & allocMask)
#define ORG_BITMAP(var) ((~((var)&allocMask)) & allocMask)
#define PROBE_BITMAP(var) ((var)&allocMask)

struct Bucket {
  inline int find_empty_slot() {
    if (GET_COUNT(bitmap) == kNumPairPerBucket) {
      return -1;
    }
    auto mask = ~(GET_BITMAP(bitmap));  // Now the 1 bit should be the empty
                                        // slot
    return __builtin_ctz(mask);
  }

  /*true indicates overflow, needs extra check in the stash*/
  inline bool test_overflow() { return (overflowCount != 0) ? true : false; }

  inline bool test_stash_check() {
    int mask = *((int *)membership);
    return ((mask & overflowSet) != 0) ? true : false;
  }

  inline void clear_stash_check() {
    int mask = *((int *)membership);
    *((int *)membership) = (*((int *)membership)) & (~overflowSet);
  }

  inline void set_indicator(uint8_t meta_hash, Bucket *neighbor, uint8_t pos) {
    int mask = finger_array[14];
    mask = ~mask;
    auto index = __builtin_ctz(mask);

    if (index < 4) {
      finger_array[15 + index] = meta_hash;
      finger_array[14] =
          ((uint8_t)(1 << index) | finger_array[14]); /*may be optimized*/
      finger_array[19] =
          (finger_array[19] & (~(3 << (index * 2)))) | (pos << (index * 2));
    } else {
      mask = neighbor->finger_array[14];
      mask = ~mask;
      index = __builtin_ctz(mask);
      if (index < 4) {
        neighbor->finger_array[15 + index] = meta_hash;
        neighbor->finger_array[14] =
            ((uint8_t)(1 << index) | neighbor->finger_array[14]);
        neighbor->overflowMember =
            ((uint8_t)(1 << index) | neighbor->overflowMember);
        neighbor->finger_array[19] =
            (neighbor->finger_array[19] & (~(3 << (index * 2)))) |
            (pos << (index * 2));
      } else { /*overflow, increase count*/
        overflowCount++;
      }
    }
    *((int *)membership) = (*((int *)membership)) | overflowSet;
  }

  /*both clear this bucket and its neighbor bucket*/
  inline void unset_indicator(uint8_t meta_hash, Bucket *neighbor, Key_t key,
                              uint64_t pos) {
    /*also needs to ensure that this meta_hash must belongs to other bucket*/
    bool clear_success = false;
    int mask1 = finger_array[14];
    for (int i = 0; i < 4; ++i) {
      if (CHECK_BIT(mask1, i) && (finger_array[15 + i] == meta_hash) &&
          (((1 << i) & overflowMember) == 0) &&
          (((finger_array[19] >> (2 * i)) & stashMask) == pos)) {
        // printf("clear the indicator 1\n");
        finger_array[14] = finger_array[14] & ((uint8_t)(~(1 << i)));
        finger_array[19] = finger_array[19] & (~(3 << (i * 2)));
        assert(((finger_array[19] >> (i * 2)) & stashMask) == 0);
        clear_success = true;
        break;
      }
    }

    int mask2 = neighbor->finger_array[14];
    if (!clear_success) {
      for (int i = 0; i < 4; ++i) {
        if (CHECK_BIT(mask2, i) &&
            (neighbor->finger_array[15 + i] == meta_hash) &&
            (((1 << i) & neighbor->overflowMember) != 0) &&
            (((neighbor->finger_array[19] >> (2 * i)) & stashMask) == pos)) {
          // printf("clear the indicator 2\n");
          neighbor->finger_array[14] =
              neighbor->finger_array[14] & ((uint8_t)(~(1 << i)));
          neighbor->overflowMember =
              neighbor->overflowMember & ((uint8_t)(~(1 << i)));
          neighbor->finger_array[19] =
              neighbor->finger_array[19] & (~(3 << (i * 2)));
          assert(((neighbor->finger_array[19] >> (i * 2)) & stashMask) == 0);
          clear_success = true;
          break;
        }
      }
    }

    if (!clear_success) {
      // printf("decrease overflowCount\n");
      overflowCount--;
    }

    mask1 = finger_array[14];
    mask2 = neighbor->finger_array[14];
    if (((mask1 & (~overflowMember)) == 0) && (overflowCount == 0) &&
        ((mask2 & neighbor->overflowMember) == 0)) {
      // printf("clear the stash check\n");
      clear_stash_check();
    }
  }

  int unique_check(uint8_t meta_hash, Key_t key, Bucket *neighbor,
                   Bucket *stash) {
    // return check_and_get(meta_hash, key) == NONE ? 0 : -1;
    if ((check_and_get(meta_hash, key, false) != NONE) ||
        (neighbor->check_and_get(meta_hash, key, true) != NONE)) {
      return -1;
    }

    if (test_stash_check()) {
      auto test_stash = false;
      if (test_overflow()) {
        test_stash = true;
      } else {
        int mask = finger_array[14];
        if (finger_array[14] != 0) {
          for (int i = 0; i < 4; ++i) {
            if (CHECK_BIT(mask, i) && (finger_array[15 + i] == meta_hash) &&
                (((1 << i) & overflowMember) == 0)) {
              test_stash = true;
              goto STASH_CHECK;
            }
          }
        }

        if (neighbor->finger_array[14] != 0) {
          mask = neighbor->finger_array[14];
          for (int i = 0; i < 4; ++i) {
            if (CHECK_BIT(mask, i) &&
                (neighbor->finger_array[15 + i] == meta_hash) &&
                (((1 << i) & neighbor->overflowMember) != 0)) {
              test_stash = true;
              break;
            }
          }
        }
      }
    STASH_CHECK:
      if (test_stash == true) {
        for (int i = 0; i < stashBucket; ++i) {
          Bucket *curr_bucket = stash + i;
          if (curr_bucket->check_and_get(meta_hash, key, false) != NONE) {
            return -1;
          }
        }
      }
    }
    return 0;
  }

  Value_t check_and_get(uint8_t meta_hash, Key_t key, bool probe) {
    int mask = 0;
    SSE_CMP8(finger_array, meta_hash);
    if (!probe) {
      mask = mask & GET_BITMAP(bitmap) & ((~(*(int *)membership)) & allocMask);
    } else {
      mask = mask & GET_BITMAP(bitmap) & ((*(int *)membership) & allocMask);
    }

    /*loop unrolling*/
    if (mask != 0) {
      for (int i = 0; i < 12; i += 4) {
        if (CHECK_BIT(mask, i) && (_[i].key == key)) {
          return _[i].value;
        }

        if (CHECK_BIT(mask, i + 1) && (_[i + 1].key == key)) {
          return _[i + 1].value;
        }

        if (CHECK_BIT(mask, i + 2) && (_[i + 2].key == key)) {
          return _[i + 2].value;
        }

        if (CHECK_BIT(mask, i + 3) && (_[i + 3].key == key)) {
          return _[i + 3].value;
        }
      }

      if (CHECK_BIT(mask, 12) && (_[12].key == key)) {
        return _[12].value;
      }

      if (CHECK_BIT(mask, 13) && (_[13].key == key)) {
        return _[13].value;
      }
    }
    return NONE;
  }

  inline void set_hash(int index, uint8_t meta_hash,
                       bool probe) /* Do I needs the atomic instruction????*/
  {
    finger_array[index] = meta_hash;
    auto new_bitmap = bitmap | (1 << (index + 4));
    assert(GET_COUNT(bitmap) < kNumPairPerBucket);
    new_bitmap += 1;
    bitmap = new_bitmap;
// #ifdef PMEM
    // Allocator::NTWrite32(reinterpret_cast<uint32_t *>(&bitmap), new_bitmap);
// #else
    // bitmap = new_bitmap;
// #endif
    if (probe) {
      *((int *)membership) = (1 << index) | *((int *)membership);
    }
  }

  inline uint8_t get_hash(int index) { return finger_array[index]; }

  inline void unset_hash(int index, bool nt_flush = false) {
    auto new_bitmap = bitmap & (~(1 << (index + 4)));
    assert(GET_COUNT(bitmap) <= kNumPairPerBucket);
    assert(GET_COUNT(bitmap) > 0);
    new_bitmap -= 1;
#ifdef PMEM
    if (nt_flush) {
      Allocator::NTWrite32(reinterpret_cast<uint32_t *>(&bitmap), new_bitmap);
    } else {
      bitmap = new_bitmap;
    }
#else
    bitmap = new_bitmap;
#endif

    *((int *)membership) =
        (~(1 << index)) &
        (*((int *)membership)); /*since they are in the same cacheline,
                                   therefore no performance influence?*/
  }

  inline void get_lock() {
    auto old_value = version_lock & lockMask;
    auto new_value = version_lock | lockSet;
    while (!CAS(&version_lock, &old_value, new_value)) {
      old_value = version_lock & lockMask;
      new_value = version_lock | lockSet;
    }
  }

  inline bool try_get_lock() {
    auto old_value = version_lock & lockMask;
    auto new_value = version_lock | lockSet;
    return CAS(&version_lock, &old_value, new_value);
  }

  inline void release_lock() {
    auto old_value = version_lock;
    auto new_value = ((old_value & lockMask) + 1) & lockMask;

    while (!CAS(&version_lock, &old_value, new_value)) {
      old_value = version_lock;
      new_value = ((old_value & lockMask) + 1) & lockMask;
    }
  }

  /*if the lock is set, return true*/
  inline bool test_lock_set(uint32_t &version) {
    auto value = __atomic_load_n(&version_lock, __ATOMIC_ACQUIRE);
    version = value & lockMask;
    return (value & lockSet) != 0;
  }

  // test whether the version has change, if change, return true
  inline bool test_lock_version_change(uint32_t old_version) {
    auto value = __atomic_load_n(&version_lock, __ATOMIC_ACQUIRE);
    auto version = value & lockMask;
    return ((value & lockSet) != 0) || (version != old_version);
  }

  int Insert(Key_t key, Value_t value, uint8_t meta_hash, bool probe) {
    auto slot = find_empty_slot();
    /* this branch can be optimized out*/
    assert(slot < kNumPairPerBucket);
    if (slot == -1) {
      printf("cannot find the empty slot, for key %llu\n", key);
      return -1;
    }
    _[slot].value = value;
    _[slot].key = key;
#ifdef PMEM
    Allocator::Persist(&_[slot], sizeof(_[slot]));
#endif
    mfence();
    set_hash(slot, meta_hash, probe);
    return 0;
  }

  /*if delete success, then return 0, else return -1*/
  int Delete(Key_t key, uint8_t meta_hash, bool probe) {
    /*do the simd and check the key, then do the delete operation*/
    int mask = 0;
    SSE_CMP8(finger_array, meta_hash);
    if (!probe) {
      mask = mask & GET_BITMAP(bitmap) & ((~(*(int *)membership)) & allocMask);
    } else {
      mask = mask & GET_BITMAP(bitmap) & ((*(int *)membership) & allocMask);
    }
    /*loop unrolling*/
    if (mask != 0) {
      for (int i = 0; i < 12; i += 4) {
        if (CHECK_BIT(mask, i) && (_[i].key == key)) {
          unset_hash(i, false);
          return 0;
        }

        if (CHECK_BIT(mask, i + 1) && (_[i + 1].key == key)) {
          unset_hash(i + 1, false);
          return 0;
        }

        if (CHECK_BIT(mask, i + 2) && (_[i + 2].key == key)) {
          unset_hash(i + 2, false);
          return 0;
        }

        if (CHECK_BIT(mask, i + 3) && (_[i + 3].key == key)) {
          unset_hash(i + 3, false);
          return 0;
        }
      }

      if (CHECK_BIT(mask, 12) && (_[12].key == key)) {
        unset_hash(12, false);
        return 0;
      }

      if (CHECK_BIT(mask, 13) && (_[13].key == key)) {
        unset_hash(13, false);
        return 0;
      }
    }
    return -1;
  }

  int Insert_with_noflush(Key_t key, Value_t value, uint8_t meta_hash,
                          bool probe) {
    auto slot = find_empty_slot();
    /* this branch can be optimized out*/
    assert(slot < kNumPairPerBucket);
    if (slot == -1) {
      printf("cannot find the empty slot, for key %llu\n", key);
      return -1;
    }
    _[slot].value = value;
    _[slot].key = key;
    set_hash(slot, meta_hash, probe);
    return 0;
  }

  void Insert_displace(Key_t key, Value_t value, uint8_t meta_hash, int slot,
                       bool probe) {
    _[slot].value = value;
    _[slot].key = key;
#ifdef PMEM
    Allocator::Persist(&_[slot], sizeof(_Pair));
#endif
    mfence();
    set_hash(slot, meta_hash, probe);
  }

  void Insert_displace_with_noflush(Key_t key, Value_t value, uint8_t meta_hash,
                                    int slot, bool probe) {
    _[slot].value = value;
    _[slot].key = key;
    set_hash(slot, meta_hash, probe);
  }

  /* Find the displacment element in this bucket*/
  /*
  int Find_displacement(int x){
          for (int i = 0; i < kNumPairPerBucket; ++i)
          {
                  auto key_hash = h(&_[i], sizeof(Key_t));
                  auto y = BUCKET_INDEX(key_hash);
                  if (x == y)
                  {
                          return i;
                  }
          }
          return -1;
  }*/

  inline int Find_org_displacement() {
    int mask = (~(*((int *)membership))) & allocMask;
    if (mask == 0) {
      return -1;
    }
    return __builtin_ctz(mask);
  }

  /*find element that it is in the probe*/
  inline int Find_probe_displacement() {
    int mask = (*((int *)membership)) & allocMask;
    if (mask == 0) {
      return -1;
    }
    return __builtin_ctz(mask);
  }

  	/*suite of function to set/unset the flush state of the node*/
	inline void setPreNonFlush(){
    overflowMember = overflowMember | preNeighborSet;
	}

	inline void unsetPreNonFlush(){
    overflowMember = overflowMember & (~preNeighborSet);
	}

	/* If the non-flush is set, return true*/
	inline bool testPreNonFlush(){
		return ((overflowMember & preNeighborSet) != 0);
	}

	inline void setNextNonFlush(){
		overflowMember = overflowMember | nextNeighborSet;
	}

	inline void unsetNextNonFlush(){
		overflowMember = overflowMember & (~nextNeighborSet);
	}

	inline bool testNextNonFlush(){
		return ((overflowMember & nextNeighborSet) != 0);
	}

  uint32_t version_lock;
  int bitmap;               // allocation bitmap + pointer bitmao + counter
  uint8_t finger_array[20]; /*only use the first 14 bytes, can be accelerated by
                               SSE instruction,0-13 for finger, 14-17 for
                               overflowed, 18 as the bitmap, 19 as the overflow bucket index indicator*/
  uint8_t membership[2];    /*Used to test whether the key originally belongs to
                               this bucket*/
  uint8_t overflowMember; /*overflowmember indicates membership of the overflow
                             fingerprint*/
  uint8_t overflowCount;

  _Pair _[kNumPairPerBucket];
};

struct Table;

struct Directory {
  Table **_;
  size_t global_depth;
  size_t version;
  size_t depth_count;
  Directory(size_t capacity, size_t _version, Table **tables) {
    version = _version;
    global_depth = static_cast<size_t>(log2(capacity));
    _ = tables;
    depth_count = 0;
  }

  static void New(Directory **dir, size_t capacity, size_t version) {
    Table **tables{nullptr};
    Allocator::ZAllocate((void **)&tables, kCacheLineSize,
                         sizeof(uint64_t) * capacity);
    // tables = reinterpret_cast<Table**>(malloc(sizeof(uint64_t) * capacity));
#ifdef PMEM
    auto callback = [](PMEMobjpool *pool, void *ptr, void *arg) {
      auto value_ptr =
          reinterpret_cast<std::tuple<size_t, size_t, Table **> *>(arg);
      auto dir_ptr = reinterpret_cast<Directory *>(ptr);
      dir_ptr->version = std::get<1>(*value_ptr);
      dir_ptr->global_depth =
          static_cast<size_t>(log2(std::get<0>(*value_ptr)));
      dir_ptr->_ = std::get<2>(*value_ptr);
      return 0;
    };
    std::tuple callback_args = {capacity, version, tables};
    Allocator::Allocate((void **)dir, kCacheLineSize, sizeof(Directory),
                        callback, reinterpret_cast<void *>(&callback_args));
#else
    Allocator::ZAllocate((void **)dir, kCacheLineSize, sizeof(Directory));
    new (*dir) Directory(capacity, version, tables);
#endif
  }
};

/* the meta hash-table referenced by the directory*/
struct Table {
  static void New(Table **tbl, size_t depth, Table *pp) {
#ifdef PMEM
    auto callback = [](PMEMobjpool *pool, void *ptr, void *arg) {
      auto value_ptr = reinterpret_cast<std::pair<size_t, Table *> *>(arg);
      auto table_ptr = reinterpret_cast<Table *>(ptr);
      memset(ptr, 0, sizeof(Table));
      table_ptr->local_depth = value_ptr->first;
      table_ptr->next = value_ptr->second;
      pmemobj_persist(pool, ptr, sizeof(Table));
      return 0;
    };
    std::pair callback_para(depth, pp);
    Allocator::Allocate((void **)tbl, kCacheLineSize, sizeof(Table), callback,
                        reinterpret_cast<void *>(&callback_para));
#else
    Allocator::ZAllocate((void **)tbl, kCacheLineSize, sizeof(Table));
    (*tbl)->local_depth = depth;
    (*tbl)->next = pp;
#endif
  };

  ~Table(void) {}

  int Insert(Key_t key, Value_t value, size_t key_hash, uint8_t meta_hash,
             Directory **);
  void Insert4split(Key_t key, Value_t value, size_t key_hash,
                    uint8_t meta_hash);
  Table *Split(size_t);
  int Delete(Key_t key, size_t key_hash, uint8_t meta_hash, Directory **_dir);
  bool Acquire_and_verify(size_t _pattern) {
    if (bucket->try_get_lock()) {
      if (pattern == _pattern) {
        return true;
      } else {
        bucket->release_lock();
        return false;
      }
    }
    return false;
  }

  bool All_acquire_and_verify() {
    Bucket *curr_bucket;
#ifndef COUNTING
    if (GET_COUNT(bucket->bitmap) != 0) {
      return false;
    }
#endif
    for (int i = 1; i < kNumBucket + 1; ++i) {
      curr_bucket = bucket + i;
      curr_bucket->get_lock();
#ifndef COUNTING
      if (GET_COUNT(curr_bucket->bitmap) != 0)
#else
      if (number != 0)
#endif
      {
        for (int j = i; j > 0; --j) {
          curr_bucket = bucket + j;
          curr_bucket->release_lock();
        }
        return false;
      }
    }

    return true;
  }

  void All_release() {
    Bucket *curr_bucket;
    for (int i = kNumBucket; i >= 0; --i) {
      curr_bucket = bucket + i;
      curr_bucket->release_lock();
    }
  }

  bool Empty_verify() {
#ifndef COUNTING
    Bucket *curr_bucket;
    for (int i = 0; i < kNumBucket + 1; ++i) {
      curr_bucket = bucket + i;
      if (GET_COUNT(curr_bucket->bitmap) != 0) {
        return false;
      }
    }
    return true;
#else
    return (number == 0);
#endif
  }

  int Next_displace(Bucket* target, Bucket *neighbor, Bucket *next_neighbor, Key_t key,
                    Value_t value, uint8_t meta_hash) {
    int displace_index = neighbor->Find_org_displacement();
    if ((GET_COUNT(next_neighbor->bitmap) != kNumPairPerBucket) &&
        (displace_index != -1)) {
      // printf("do the displacement in next bucket, the displaced key is %lld,
      // the new key is %lld\n", neighbor->_[displace_index].key, key);
      next_neighbor->Insert(neighbor->_[displace_index].key,
                            neighbor->_[displace_index].value,
                            neighbor->finger_array[displace_index], true);
      neighbor->setNextNonFlush();
      next_neighbor->release_lock();
#ifdef PMEM
      Allocator::Persist(&next_neighbor->bitmap, sizeof(next_neighbor->bitmap));
#endif
      neighbor->unsetNextNonFlush();
      neighbor->unset_hash(displace_index);
      neighbor->Insert_displace(key, value, meta_hash, displace_index, true);
      target->setNextNonFlush();
      neighbor->release_lock();
#ifdef PMEM
      Allocator::Persist(&neighbor->bitmap, sizeof(neighbor->bitmap));
#endif
      target->unsetNextNonFlush();
      target->release_lock();
#ifdef COUNTING
      __sync_fetch_and_add(&number, 1);
#endif
      return 0;
    }
    return -1;
  }

  int Prev_displace(Bucket *target, Bucket *prev_neighbor, Bucket *neighbor, Key_t key,
                    Value_t value, uint8_t meta_hash) {
    int displace_index = target->Find_probe_displacement();
    if ((GET_COUNT(prev_neighbor->bitmap) != kNumPairPerBucket) &&
        (displace_index != -1)) {
      // printf("do the displacement in previous bucket,the displaced key is
      // %lld, the new key is %lld\n", target->_[displace_index].key, key);
      prev_neighbor->Insert(target->_[displace_index].key,
                            target->_[displace_index].value,
                            target->finger_array[displace_index], false);
      target->setPreNonFlush();
      prev_neighbor->release_lock();
#ifdef PMEM
      Allocator::Persist(&prev_neighbor->bitmap, sizeof(prev_neighbor->bitmap));
#endif
      target->unsetPreNonFlush();
      target->unset_hash(displace_index);
      target->Insert_displace(key, value, meta_hash, displace_index, false);
      neighbor->setPreNonFlush();
      target->release_lock();
#ifdef PMEM
      Allocator::Persist(&target->bitmap, sizeof(target->bitmap));
#endif
      neighbor->unsetPreNonFlush();
      neighbor->release_lock();
#ifdef COUNTING
      __sync_fetch_and_add(&number, 1);
#endif
      return 0;
    }
    return -1;
  }

  int Stash_insert(Bucket *target, Bucket *neighbor, Key_t key, Value_t value,
                   uint8_t meta_hash, int stash_pos) {
    for (int i = 0; i < stashBucket; ++i) {
      Bucket *curr_bucket = bucket + kNumBucket + ((stash_pos + i) & stashMask);
      if (GET_COUNT(curr_bucket->bitmap) < kNumPairPerBucket) {
        // printf("insertion in the stash for key %lld\n", key);
        curr_bucket->Insert(key, value, meta_hash, false);
#ifdef PMEM
        Allocator::Persist(&curr_bucket->bitmap, sizeof(curr_bucket->bitmap));
#endif
        target->set_indicator(meta_hash, neighbor, (stash_pos + i) & stashMask);
#ifdef COUNTING
        __sync_fetch_and_add(&number, 1);
#endif
        return 0;
      }
    }
    return -1;
  }

  char dummy[48];
  struct Bucket bucket[kNumBucket + stashBucket];
  size_t local_depth;
  size_t pattern;
  int number;
  Table *next;
  int state; /*-1 means this bucket is merging, -2 means this bucket is
                splitting, so we cannot count the depth_count on it during
                scanning operation*/
};

/* it needs to verify whether this bucket has been deleted...*/
int Table::Insert(Key_t key, Value_t value, size_t key_hash, uint8_t meta_hash,
                  Directory **_dir) {
RETRY:
  /*we need to first do the locking and then do the verify*/
  auto y = BUCKET_INDEX(key_hash);
  Bucket *target = bucket + y;
  Bucket *neighbor = bucket + ((y + 1) & bucketMask);
  // printf("for key %lld, target bucket is %lld, meta_hash is %d\n", key,
  // BUCKET_INDEX(key_hash), meta_hash);
  target->get_lock();
  if (!neighbor->try_get_lock()) {
    target->release_lock();
    return -2;
  }

  auto old_sa = *_dir;
  auto x = (key_hash >> (8 * sizeof(key_hash) - old_sa->global_depth));
  if (old_sa->_[x] != this) /* verify process*/
  {
    neighbor->release_lock();
    target->release_lock();
    return -2;
  }

  /*unique check, needs to check 2 hash table*/
  auto ret =
      target->unique_check(meta_hash, key, neighbor, bucket + kNumBucket);
  if (ret == -1) {
    neighbor->release_lock();
    target->release_lock();
    return 0;
  }

  if (((GET_COUNT(target->bitmap)) == kNumPairPerBucket) &&
      ((GET_COUNT(neighbor->bitmap)) == kNumPairPerBucket)) {
      Bucket *next_neighbor = bucket + ((y + 2) & bucketMask);
      // Next displacement
      if (!next_neighbor->try_get_lock()) {
        neighbor->release_lock();
        target->release_lock();
        return -2;
      }
      auto ret = Next_displace(target, neighbor, next_neighbor, key, value, meta_hash);
      if (ret == 0) {
        return 0;
      }
      next_neighbor->release_lock();

      Bucket *prev_neighbor;
      int prev_index;
      if (y == 0) {
        prev_neighbor = bucket + kNumBucket - 1;
        prev_index = kNumBucket - 1;
      } else {
        prev_neighbor = bucket + y - 1;
        prev_index = y - 1;
      }
      if (!prev_neighbor->try_get_lock()) {
        target->release_lock();
        neighbor->release_lock();
        return -2;
      }

      ret = Prev_displace(target, prev_neighbor, neighbor,key, value, meta_hash);
      if (ret == 0) {
        return 0;
      }

      Bucket *stash = bucket + kNumBucket;
      if (!stash->try_get_lock()) {
        neighbor->release_lock();
        target->release_lock();
        prev_neighbor->release_lock();
        return -2;
      }
      ret =
          Stash_insert(target, neighbor, key, value, meta_hash, y & stashMask);

      stash->release_lock();
      neighbor->release_lock();
      target->release_lock();
      prev_neighbor->release_lock();
      return ret;
  }

  if (GET_COUNT(target->bitmap) <= GET_COUNT(neighbor->bitmap)) {
    target->Insert(key, value, meta_hash, false);
    neighbor->setPreNonFlush();
    target->release_lock();
#ifdef PMEM
    Allocator::Persist(&target->bitmap, sizeof(target->bitmap));
#endif
    neighbor->unsetPreNonFlush();
    neighbor->release_lock();
  } else {
    neighbor->Insert(key, value, meta_hash, true);
    target->setNextNonFlush();
    neighbor->release_lock();
#ifdef PMEM
    Allocator::Persist(&neighbor->bitmap, sizeof(neighbor->bitmap));
#endif
    target->unsetNextNonFlush();
    target->release_lock();
  }
#ifdef COUNTING
  __sync_fetch_and_add(&number, 1);
#endif
  return 0;
}

/*the insert needs to be perfectly balanced, not destory the power of balance*/
void Table::Insert4split(Key_t key, Value_t value, size_t key_hash,
                         uint8_t meta_hash) {
  auto y = BUCKET_INDEX(key_hash);
  Bucket *target = bucket + y;
  Bucket *neighbor = bucket + ((y + 1) & bucketMask);
  // auto insert_target =
  // (target->count&lowCountMask)<=(neighbor->count&lowCountMask)?target:neighbor;
  Bucket *insert_target;
  bool probe = false;
  if (GET_COUNT(target->bitmap) <= GET_COUNT(neighbor->bitmap)) {
    insert_target = target;
  } else {
    insert_target = neighbor;
    probe = true;
  }

  // assert(insert_target->count < kNumPairPerBucket);
  /*some bucket may be overflowed?*/
  if (GET_COUNT(insert_target->bitmap) < kNumPairPerBucket) {
    insert_target->_[GET_COUNT(insert_target->bitmap)].key = key;
    insert_target->_[GET_COUNT(insert_target->bitmap)].value = value;
    insert_target->set_hash(GET_COUNT(insert_target->bitmap), meta_hash, probe);
#ifdef COUNTING
    ++number;
#endif
  } else {
    /*do the displacement or insertion in the stash*/
    Bucket *next_neighbor = bucket + ((y + 2) & bucketMask);
    int displace_index;
    displace_index = neighbor->Find_org_displacement();
    if (((GET_COUNT(next_neighbor->bitmap)) != kNumPairPerBucket) &&
        (displace_index != -1)) {
      // printf("do the displacement in next bucket, the displaced key is %lld,
      // the new key is %lld\n", neighbor->_[displace_index].key, key);
      next_neighbor->Insert_with_noflush(
          neighbor->_[displace_index].key, neighbor->_[displace_index].value,
          neighbor->finger_array[displace_index], true);
      neighbor->unset_hash(displace_index);
      neighbor->Insert_displace_with_noflush(key, value, meta_hash,
                                             displace_index, true);
#ifdef COUNTING
      ++number;
#endif
      return;
    }
    Bucket *prev_neighbor;
    int prev_index;
    if (y == 0) {
      prev_neighbor = bucket + kNumBucket - 1;
      prev_index = kNumBucket - 1;
    } else {
      prev_neighbor = bucket + y - 1;
      prev_index = y - 1;
    }

    displace_index = target->Find_probe_displacement();
    if (((GET_COUNT(prev_neighbor->bitmap)) != kNumPairPerBucket) &&
        (displace_index != -1)) {
      // printf("do the displacement in previous bucket,the displaced key is
      // %lld, the new key is %lld\n", target->_[displace_index].key, key);
      prev_neighbor->Insert_with_noflush(
          target->_[displace_index].key, target->_[displace_index].value,
          target->finger_array[displace_index], false);
      target->unset_hash(displace_index);
      target->Insert_displace_with_noflush(key, value, meta_hash,
                                           displace_index, false);
#ifdef COUNTING
      ++number;
#endif
      return;
    }

    Stash_insert(target, neighbor, key, value, meta_hash, y & stashMask);
  }
}

Table *Table::Split(size_t _key_hash) {
  size_t new_pattern = (pattern << 1) + 1;
  size_t old_pattern = pattern << 1;

  Bucket *curr_bucket;
  for (int i = 1; i < kNumBucket; ++i) {
    curr_bucket = bucket + i;
    curr_bucket->get_lock();
  }
  // printf("my pattern is %lld, my load factor is %f\n", pattern,
  // ((double)number)/(kNumBucket*kNumPairPerBucket+kNumPairPerBucket));
  state = -2;

  Table::New(&next, local_depth + 1, next);

  next->state = -2;
  next->bucket->get_lock(); /* get the first lock of the new bucket to avoid it
                               is operated(split or merge) by other threads*/
  size_t key_hash;
  for (int i = 0; i < kNumBucket; ++i) {
    curr_bucket = bucket + i;
    auto mask = GET_BITMAP(curr_bucket->bitmap);
    for (int j = 0; j < kNumPairPerBucket; ++j) {
      if (CHECK_BIT(mask, j)) {
        key_hash = h(&(curr_bucket->_[j].key), sizeof(Key_t));
        if ((key_hash >> (64 - local_depth - 1)) == new_pattern) {
          next->Insert4split(
              curr_bucket->_[j].key, curr_bucket->_[j].value, key_hash,
              curr_bucket->finger_array[j]); /*this shceme may destory the
                                                balanced segment*/
          curr_bucket->unset_hash(j);
#ifdef COUNTING
          number--;
#endif
        }
      }
    }
  }

  /*split the stash bucket, the stash must be full, right?*/
  for (int i = 0; i < stashBucket; ++i) {
    curr_bucket = bucket + kNumBucket + i;
    auto mask = GET_BITMAP(curr_bucket->bitmap);
    for (int j = 0; j < kNumPairPerBucket; ++j) {
      if (CHECK_BIT(mask, j)) {
        key_hash = h(&(curr_bucket->_[j].key), sizeof(Key_t));
        if ((key_hash >> (64 - local_depth - 1)) == new_pattern) {
          next->Insert4split(
              curr_bucket->_[j].key, curr_bucket->_[j].value, key_hash,
              curr_bucket->finger_array[j]); /*this shceme may destory the
                                                balanced segment*/
          auto bucket_ix = BUCKET_INDEX(key_hash);
          auto org_bucket = bucket + bucket_ix;
          auto neighbor_bucket = bucket + ((bucket_ix + 1) & bucketMask);
          org_bucket->unset_indicator(curr_bucket->finger_array[j],
                                      neighbor_bucket, curr_bucket->_[j].key,
                                      i);
          curr_bucket->unset_hash(j);
#ifdef COUNTING
          number--;
#endif
        }
      }
    }
  }
  next->pattern = new_pattern;
  pattern = old_pattern;

#ifdef PMEM
  Allocator::Persist(next, sizeof(Table));
#endif
  return next;
}

// Do the delete operation in the small hash table, merge/directory halving if
// necessary
/*for the return value, -2 means needs retry, 0 means delete success, 1 means
 * delete success and also the bucket is empty, -1 means the deletion fails*/
 /*
int Table::Delete(Key_t key, size_t key_hash, uint8_t meta_hash,
                  Directory **_dir) {
RETRY:
  auto y = BUCKET_INDEX(key_hash);
  Bucket *target = bucket + y;
  Bucket *neighbor = bucket + ((y + 1) & bucketMask);
  target->get_lock();

  auto old_sa = *_dir;
  auto x = (key_hash >> (8 * sizeof(key_hash) - old_sa->global_depth));
  if (old_sa->_[x] != this) 
  {
    target->release_lock();
    return -2;
  }

  auto ret = target->Delete(key, meta_hash, false);
  if (ret == 0) {
    target->release_lock();
    return 0;
  }

  if (!neighbor->try_get_lock()) {
    target->release_lock();
    return -2;
  }

  ret = neighbor->Delete(key, meta_hash, true);
  if (ret == 0) {
    neighbor->release_lock();
    target->release_lock();
    return 0;
  }

  Bucket *stash = bucket + kNumBucket;
  if (target->test_stash_check()) {
    auto test_stash = false;
    if (target->test_overflow()) {
      test_stash = true;
    } else {
      int mask = target->finger_array[14];

      if (mask != 0) {
        for (int i = 0; i < 4; ++i) {
          if (CHECK_BIT(mask, i) &&
              (target->finger_array[15 + i] == meta_hash) &&
              (((1 << i) & target->overflowMember) == 0)) {
            test_stash = true;
            break;
          }
        }
      }

      mask = neighbor->finger_array[14];
      if (mask != 0) {
        for (int i = 0; i < 4; ++i) {
          if (CHECK_BIT(mask, i) &&
              (neighbor->finger_array[15 + i] == meta_hash) &&
              (((1 << i) & neighbor->overflowMember) != 0)) {
            test_stash = true;
            break;
          }
        }
      }
    }

    if (test_stash) {
      Bucket *curr_bucket;
      stash->get_lock();
      for (int i = 0; i < stashBucket; ++i) {
        curr_bucket = stash + ((i + (y & stashMask)) & stashMask);
        ret = curr_bucket->Delete(key, meta_hash, false);
        if (ret == 0) {
          target->unset_indicator(meta_hash, neighbor, key, i);
          stash->release_lock();
          neighbor->release_lock();
          target->release_lock();
          return 0;
        }
      }
      stash->release_lock();
    }
  }
  neighbor->release_lock();
  target->release_lock();
  return -1; 
}
*/

class Finger_EH {
 public:
  Finger_EH(void);
  Finger_EH(size_t);
  ~Finger_EH(void);
  int Insert(Key_t key, Value_t value);
  bool InsertOnly(Key_t, Value_t);
  bool Delete(Key_t);
  Value_t Get(Key_t);
  void Directory_Doubling(int x, Table *new_b);
  void Directory_Update(Directory *_sa, int x, Table *new_b);
  void Halve_Directory();
  void Lock_Directory();
  void Unlock_Directory();
  int FindAnyway(Key_t key);
  void CheckDepthCount();
  void getNumber() {
    // printf("the size of the bucket is %lld\n", sizeof(struct Bucket));
    // printf("the size of the Table is %lld\n", sizeof(struct Table));

    size_t _count = 0;
    size_t seg_count = 0;
    Directory *seg = dir;
    Table **dir_entry = seg->_;
    Table *ss;
    auto global_depth = seg->global_depth;
    size_t depth_diff;
    int capacity = pow(2, global_depth);
    for (int i = 0; i < capacity;) {
      ss = dir_entry[i];
      depth_diff = global_depth - ss->local_depth;
      _count += ss->number;
      seg_count++;
      i += pow(2, depth_diff);
    }
    printf("#items: %lld\n", _count);
    // printf("the segment number in this hash table is %lld\n", seg_count);
    printf("load_factor: %f\n",
           (double)_count / (seg_count * kNumPairPerBucket * (kNumBucket + 2)));
    printf("Raw_Space: %f\n",
           (double)(_count * 16) / (seg_count * sizeof(Table)));
  }

  inline bool Acquire(void) {
    int unlocked = 0;
    return CAS(&lock, &unlocked, 1);
  }

  inline bool Release(void) {
    int locked = 1;
    return CAS(&lock, &locked, 0);
  }

  inline int Test_Directory_Lock_Set(void) {
    return __atomic_load_n(&lock, __ATOMIC_ACQUIRE);
  }

  Directory *dir;
  int lock;
#ifdef PMEM
  PMEMobjpool *pool_addr;

  /* directory allocation will write to here first,
   * in oder to perform safe directory allocation
   * */
  Directory *alloc_dir;
#endif
};

Finger_EH::Finger_EH(void) {}

Finger_EH::Finger_EH(size_t initCap) {
  Directory::New(&dir, initCap, 0);
  lock = 0;

  Table::New(dir->_ + initCap - 1, dir->global_depth, nullptr);
  dir->_[initCap - 1]->pattern = initCap - 1;
  /* Initilize the Directory*/
  for (int i = initCap - 2; i >= 0; --i) {
    Table::New(dir->_ + i, dir->global_depth, dir->_[i + 1]);
    dir->_[i]->pattern = i;
  }
  dir->depth_count = initCap;
}

Finger_EH::~Finger_EH(void) {
  // TO-DO
}

void Finger_EH::Lock_Directory() {
  while (!Acquire()) {
    asm("nop");
  }
}

void Finger_EH::Unlock_Directory() {
  while (!Release()) {
    asm("nop");
  }
}

void Finger_EH::Halve_Directory() {
  printf("Begin::Directory_Halving towards %lld\n", dir->global_depth);
  auto d = dir->_;

  Directory *new_dir;
#ifdef PMEM
  Directory::New(&alloc_dir, pow(2, dir->global_depth - 1), dir->version + 1);
  new_dir = alloc_dir;
#else
  Directory::New(&new_dir, pow(2, dir->global_depth - 1), dir->version + 1);
#endif

  auto _dir = new_dir->_;
  new_dir->depth_count = 0;
  auto capacity = pow(2, new_dir->global_depth);
  bool skip = false;

  for (int i = 0; i < capacity; ++i) {
    _dir[i] = d[2 * i];
    assert(d[2 * i] == d[2 * i + 1]);
    if (!skip) {
      if ((_dir[i]->local_depth == (dir->global_depth - 1)) &&
          (_dir[i]->state != -2)) {
        if (_dir[i]->state != -1) {
          /*the segment is normal, therefore count it as the depth_count*/
          new_dir->depth_count += 1;
        } else {
          /*the segment is merging, need to skip next count*/
          skip = true;
        }
      }
    } else {
      skip = false;
    }
  }

#ifdef PMEM
  Allocator::Persist(new_dir, sizeof(Directory));
  Allocator::NTWrite64(reinterpret_cast<uint64_t *>(dir),
                       reinterpret_cast<uint64_t>(new_dir));
  alloc_dir = nullptr;
#else
  dir = new_dir;
#endif
  printf("End::Directory_Halving towards %lld\n", dir->global_depth);
  delete d;
}

void Finger_EH::Directory_Doubling(int x, Table *new_b) {
  Table **d = dir->_;
  auto global_depth = dir->global_depth;
  printf("Directory_Doubling towards %lld\n", global_depth + 1);

  Directory *new_sa;
  Directory::New(&new_sa, 2 * pow(2, global_depth), dir->version + 1);
  auto dd = new_sa->_;

  auto capacity = pow(2, global_depth);
  for (unsigned i = 0; i < capacity; ++i) {
    dd[2 * i] = d[i];
    dd[2 * i + 1] = d[i];
  }
  dd[2 * x + 1] = new_b;
  new_sa->depth_count = 2;

#ifdef PMEM
  Allocator::Persist(new_sa, sizeof(Directory));
  Allocator::NTWrite64(reinterpret_cast<uint64_t *>(&dir),
                       reinterpret_cast<uint64_t>(new_sa));
#else
  dir = new_sa;
#endif

  /*
  FixMe
  need safely deallocate the old_directory
  */
  // printf("Done!!Directory_Doubling towards %lld\n", dir->global_depth);
}

void Finger_EH::Directory_Update(Directory *_sa, int x, Table *new_b) {
  // printf("directory update for %d\n", x);
  Table **dir_entry = _sa->_;
  auto global_depth = _sa->global_depth;
  unsigned depth_diff = global_depth - new_b->local_depth;
  if (depth_diff == 0) {
    if (x % 2 == 0) {
      dir_entry[x + 1] = new_b;
    } else {
      dir_entry[x] = new_b;
    }
    //_sa->depth_count += 2;
    __sync_fetch_and_add(&_sa->depth_count, 2);
  } else {
    int chunk_size = pow(2, global_depth - (new_b->local_depth - 1));
    x = x - (x % chunk_size);
    for (unsigned i = 0; i < chunk_size / 2; ++i) {
      dir_entry[x + chunk_size / 2 + i] = new_b;
    }
  }
  // printf("Done!directory update for %d\n", x);
}

int Finger_EH::Insert(Key_t key, Value_t value) {
  auto key_hash = h(&key, sizeof(key));
  auto meta_hash = ((uint8_t)(key_hash & kMask));  // the last 8 bits
RETRY:
  auto old_sa = dir;
  auto x = (key_hash >> (8 * sizeof(key_hash) - old_sa->global_depth));
  auto dir_entry = old_sa->_;
  Table *target = dir_entry[x];

  // printf("insert key %lld, x = %d, y = %d, meta_hash = %d\n", key, x,
  // BUCKET_INDEX(key_hash), meta_hash);
  auto ret = target->Insert(key, value, key_hash, meta_hash, &dir);
  if (ret == -1) {
    /*Insertion failure ,split process needs to get all the lock in this
     * segment? it has much overfead? I do not know...need to test whether this
     * has much overhead!*/
    if (!target->bucket->try_get_lock()) {
      goto RETRY;
    }

    /*verify procedure*/
    auto old_sa = dir;
    auto x = (key_hash >> (8 * sizeof(key_hash) - old_sa->global_depth));
    if (old_sa->_[x] != target) /* verify process*/
    {
      target->bucket->release_lock();
      goto RETRY;
    }

    auto new_b =
        target->Split(key_hash); /* also needs the verify..., and we use try
                                    lock for this rather than the spin lock*/
#ifdef PMEM
    Allocator::NTWrite64(&target->local_depth, target->local_depth + 1);
#else
    target->local_depth += 1;
#endif

    /* update directory*/
  REINSERT:
    // the following three statements may be unnecessary...
    old_sa = dir;
    dir_entry = old_sa->_;
    x = (key_hash >> (8 * sizeof(key_hash) - old_sa->global_depth));
    // assert(target == dir_entry[x].bucket_p);
    if (target->local_depth - 1 < old_sa->global_depth) {
      if (Test_Directory_Lock_Set()) {
        goto REINSERT;
      }
      Directory_Update(old_sa, x, new_b);
      /*after the update, I need to recheck*/
      if (Test_Directory_Lock_Set() || old_sa->version != dir->version) {
        goto REINSERT;
      }
    } else {
      Lock_Directory();
      if (old_sa->version != dir->version) {
        Unlock_Directory();
        goto REINSERT;
      }
      Directory_Doubling(x, new_b);
      Unlock_Directory();
    }
    /*release the lock for the target bucket and the new bucket*/
    target->state = 0;
    new_b->state = 0;
    Bucket *curr_bucket;
    for (int i = 0; i < kNumBucket; ++i) {
      curr_bucket = target->bucket + i;
      curr_bucket->release_lock();
    }
    curr_bucket = new_b->bucket;
    curr_bucket->release_lock();
    goto RETRY;
  } else if (ret == -2) {
    goto RETRY;
  }

  return 0;
}

Value_t Finger_EH::Get(Key_t key) {
  auto key_hash = h(&key, sizeof(key));
  auto meta_hash = ((uint8_t)(key_hash & kMask));  // the last 8 bits
RETRY:
  auto old_sa = dir;
  auto x = (key_hash >> (8 * sizeof(key_hash) - old_sa->global_depth));
  auto y = BUCKET_INDEX(key_hash);
  auto dir_entry = old_sa->_;
  Table *target = dir_entry[x];

  uint32_t old_version;
  uint32_t old_neighbor_version;
  Bucket *target_bucket = target->bucket + y;
  Bucket *neighbor_bucket = target->bucket + ((y + 1) & bucketMask);
  // printf("Get key %lld, x = %d, y = %d, meta_hash = %d\n", key, x,
  // BUCKET_INDEX(key_hash), meta_hash);

  if (target_bucket->test_lock_set(old_version) || 
      neighbor_bucket->test_lock_set(old_neighbor_version)) {
    goto RETRY;
  }

  /*verification procedure*/
  old_sa = dir;
  x = (key_hash >> (8 * sizeof(key_hash) - old_sa->global_depth));
  if (old_sa->_[x] != target) {
    goto RETRY;
  }

  auto ret = target_bucket->check_and_get(meta_hash, key, false);
  if ((ret != NONE) &&
      (!(target_bucket->test_lock_version_change(old_version)))) {
    return ret;
  }

  /*no need for verification procedure, we use the version number of
   * target_bucket to test whether the bucket has ben spliteted*/
  ret = neighbor_bucket->check_and_get(meta_hash, key, true);
  if ((ret != NONE) &&
      (!(target_bucket->test_lock_version_change(old_version)))) {
    return ret;
  }

  if (target_bucket->test_stash_check()) {
    auto test_stash = false;
    if (target_bucket->test_overflow()) {
      /*this only occur when the bucket has more key-values than 10 that are
       * overfloed int he shared bucket area, therefore it needs to search in
       * the extra bucket*/
      test_stash = true;
    } else {
      /*search in the original bucket*/
      int mask = target_bucket->finger_array[14];
      if (mask != 0) {
        for (int i = 0; i < 4; ++i) {
          if (CHECK_BIT(mask, i) &&
              (target_bucket->finger_array[15 + i] == meta_hash) &&
              (((1 << i) & target_bucket->overflowMember) == 0)) {
            // test_stash = true;
            // goto TEST_STASH;
            /*directly check stash*/
            Bucket *stash =
                target->bucket + kNumBucket +
                ((target_bucket->finger_array[19] >> (i * 2)) & stashMask);
            auto ret = stash->check_and_get(meta_hash, key, false);
            if (ret != NONE) {
              if (target_bucket->test_lock_version_change(old_version)) {
                goto RETRY;
              }
              return ret;
            }
          }
        }
      }

      mask = neighbor_bucket->finger_array[14];
      if (mask != 0) {
        for (int i = 0; i < 4; ++i) {
          if (CHECK_BIT(mask, i) &&
              (neighbor_bucket->finger_array[15 + i] == meta_hash) &&
              (((1 << i) & neighbor_bucket->overflowMember) != 0)) {
            // test_stash = true;
            // break;
            Bucket *stash =
                target->bucket + kNumBucket +
                ((neighbor_bucket->finger_array[19] >> (i * 2)) & stashMask);
            auto ret = stash->check_and_get(meta_hash, key, false);
            if (ret != NONE) {
              if (target_bucket->test_lock_version_change(old_version)) {
                goto RETRY;
              }
              return ret;
            }
          }
        }
      }
    }
  TEST_STASH:
    if (test_stash == true) {
      for (int i = 0; i < stashBucket; ++i) {
        Bucket *stash =
            target->bucket + kNumBucket + ((i + (y & stashMask)) & stashMask);
        auto ret = stash->check_and_get(meta_hash, key, false);
        if (ret != NONE) {
          if (target_bucket->test_lock_version_change(old_version)) {
            goto RETRY;
          }
          return ret;
        }
      }
    }
  }
  // printf("the x = %lld, the y = %lld, the meta_hash is %d\n", x, y,
  // meta_hash);
  return NONE;
}

/*the delete operation of the */
bool Finger_EH::Delete(Key_t key) {
  /*Basic delete operation and merge operation*/
  auto key_hash = h(&key, sizeof(key));
  auto meta_hash = ((uint8_t)(key_hash & kMask));  // the last 8 bits
RETRY:
  auto old_sa = dir;
  auto x = (key_hash >> (8 * sizeof(key_hash) - old_sa->global_depth));
  auto dir_entry = old_sa->_;
  Table *target_table = dir_entry[x];

  /*we need to first do the locking and then do the verify*/
  auto y = BUCKET_INDEX(key_hash);
  Bucket *target = target_table->bucket + y;
  Bucket *neighbor = target_table->bucket + ((y + 1) & bucketMask);
  target->get_lock();
  neighbor->get_lock();
  /*aslo needs to lock next bucket since we return merge if these two bucket are
   * both empty*/

  old_sa = dir;
  x = (key_hash >> (8 * sizeof(key_hash) - old_sa->global_depth));
  if (old_sa->_[x] != target_table) {
    target->release_lock();
    neighbor->release_lock();
    goto RETRY;
  }

  auto ret = target->Delete(key, meta_hash, false);
  if (ret == 0) {
    neighbor->setPreNonFlush();
    target->release_lock();
#ifdef PMEM
    Allocator::Persist(&target->bitmap, sizeof(target->bitmap));
#endif
    neighbor->unsetPreNonFlush();
    neighbor->release_lock();
    return true;
  }

  ret = neighbor->Delete(key, meta_hash, true);
  if (ret == 0) {
    target->setNextNonFlush();
    neighbor->release_lock();
#ifdef PMEM
    Allocator::Persist(&neighbor->bitmap, sizeof(neighbor->bitmap));
#endif
    target->unsetNextNonFlush();
    target->release_lock();
    return true;
  }

  if (target->test_stash_check()) {
    auto test_stash = false;
    if (target->test_overflow()) {
      /*this only occur when the bucket has more key-values than 10 that are
       * overfloed int he shared bucket area, therefore it needs to search in
       * the extra bucket*/
      test_stash = true;
    } else {
      /*search in the original bucket*/
      int mask = target->finger_array[14];
      if (mask != 0) {
        for (int i = 0; i < 4; ++i) {
          if (CHECK_BIT(mask, i) &&
              (target->finger_array[15 + i] == meta_hash) &&
              (((1 << i) & target->overflowMember) == 0)) {
              test_stash = true;
              goto TEST_STASH;
          }
        }
      }

      mask = neighbor->finger_array[14];
      if (mask != 0) {
        for (int i = 0; i < 4; ++i) {
          if (CHECK_BIT(mask, i) &&
              (neighbor->finger_array[15 + i] == meta_hash) &&
              (((1 << i) & neighbor->overflowMember) != 0)) {
              test_stash = true;
              break;
          }
        }
      }
    }

  TEST_STASH:
    if (test_stash == true) {
      Bucket *stash = target_table->bucket + kNumBucket;
      stash->get_lock();
      for (int i = 0; i < stashBucket; ++i) {
        Bucket* curr_stash =
            target_table->bucket + kNumBucket + ((i + (y & stashMask)) & stashMask);
        auto ret = curr_stash->Delete(key, meta_hash, false);
        if (ret == 0) {
          /*need to unset indicator in original bucket*/
          stash->release_lock();
#ifdef PMEM
          Allocator::Persist(&curr_stash->bitmap, sizeof(curr_stash->bitmap));
#endif
          neighbor->release_lock();
          target->release_lock();
          return true;
        }
      }
      stash->release_lock();
    }
  }
  neighbor->release_lock();
  target->release_lock();
  return false;
}

void Finger_EH::CheckDepthCount() {
  auto capacity = pow(2, dir->global_depth);
  auto dir_entry = dir->_;
  Table *current_table;
  int count = 0;
  for (int i = 0; i < capacity; ++i) {
    current_table = dir_entry[i];
    count += (current_table->local_depth == dir->global_depth) ? 1 : 0;
  }
  printf("calculate count = %d\n", count);
  printf("the recorded depth_count = %d\n", dir->depth_count);
}

/*DEBUG FUNCTION: search the position of the key in this table and print
 * correspongdign informantion in this table, to test whether it is correct*/
int Finger_EH::FindAnyway(Key_t key) {
  auto key_hash = h(&key, sizeof(key));
  auto meta_hash = ((uint8_t)(key_hash & kMask));
  auto x = (key_hash >> (8 * sizeof(key_hash) - dir->global_depth));

  size_t _count = 0;
  size_t seg_count = 0;
  Directory *seg = dir;
  Table **dir_entry = seg->_;
  Table *ss;
  auto global_depth = seg->global_depth;
  size_t depth_diff;
  int capacity = pow(2, global_depth);
  for (int i = 0; i < capacity;) {
    ss = dir_entry[i];
    /*search in this table*/
    Bucket *curr_bucket;
    for (int j = 0; j < kNumBucket; ++j) {
      curr_bucket = ss->bucket + j;
      auto ret = curr_bucket->check_and_get(meta_hash, key, false);
      if (ret != NONE) {
        printf("successfully find in the normal bucket\n");
        printf("the segment is %d, the bucket is %d\n", i, j);
        return 0;
      }
      ret = curr_bucket->check_and_get(meta_hash, key, true);
      if (ret != NONE) {
        printf("successfully find in the normal bucket\n");
        printf("the segment is %d, the bucket is %d\n", i, j);
        return 0;
      }
    }

    /*search in the stash*/
    for (int i = 0; i < stashBucket; ++i) {
      curr_bucket = ss->bucket + kNumBucket + i;
      auto ret = curr_bucket->check_and_get(meta_hash, key, false);
      if (ret != NONE) {
        printf("successfully find in the stash bucket\n");
        /*Print the image in the original bucket and neighbor bucket*/
        auto bucket_ix = BUCKET_INDEX(key_hash);
        auto org_bucket = ss->bucket + bucket_ix;
        auto neighbor_bucket = ss->bucket + ((bucket_ix + 1) & bucketMask);
        printf("the segment number is %d, the bucket_ix is %d\n", x, bucket_ix);

        printf("the image of org_bucket\n");
        // printf("the stash check is %d\n", org_bucket->test_stash_check());
        int mask = org_bucket->finger_array[14];
        for (int j = 0; j < 4; ++j) {
          printf(
              "the hash is %d, the pos bit is %d, the alloc bit is %d, the "
              "stash bucket info is %d, the real stash bucket info is %d\n",
              org_bucket->finger_array[15 + j],
              (org_bucket->overflowMember >> (j)) & 1,
              (org_bucket->finger_array[14] >> j) & 1,
              (org_bucket->finger_array[19] >> (j * 2)) & stashMask, i);
        }

        printf("the image of the neighbor bucket\n");
        printf("the stash check is %d\n", neighbor_bucket->test_stash_check());
        mask = neighbor_bucket->finger_array[14];
        for (int j = 0; j < 4; ++j) {
          printf(
              "the hash is %d, the pos bit is %d, the alloc bit is %d, the "
              "stash bucket info is %d, the real stash bucket info is %d\n",
              neighbor_bucket->finger_array[15 + j],
              (neighbor_bucket->overflowMember >> (j)) & 1,
              (neighbor_bucket->finger_array[14] >> j) & 1,
              (neighbor_bucket->finger_array[19] >> (j * 2)) & stashMask, i);
        }

        if (org_bucket->test_overflow()) {
          printf("the org bucket has overflowed\n");
        }
        return 0;
      }
    }

    depth_diff = global_depth - ss->local_depth;
    _count += ss->number;
    seg_count++;
    i += pow(2, depth_diff);
  }
  return -1;
}

#endif
