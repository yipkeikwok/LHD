#pragma once
#include <iostream>
#include <unordered_map>

#include "constants.hpp"
#include "bytes.hpp"
#include "repl.hpp"

namespace cache {

// Note: because candidates do not have the same size, in general:
// 
//    accesses != hits + evictions + fills     (not equal!)
// 
// A single access can result in multiple evictions to make space for
// the new object. That is, evictions != misses.
// 
// Fills is the number of misses that don't require an eviction, that
// is there is sufficient available space to fit the object. Evictions
// is the number of evictions, _not_ the number of misses that lead to
// an eviction.
struct Cache {
	// repl set up in cache.cpp::main()
	//	_cache->repl = repl::Policy::create(_cache, root);
  repl::Policy* repl;
  uint64_t hits;
  uint64_t misses;
  uint64_t compulsoryMisses;
  uint64_t fills;
  uint64_t evictions;
  uint64_t accessesTriggeringEvictions;
  uint64_t missesTriggeringEvictions;
  uint64_t cumulativeAllocatedSpace;
  uint64_t cumulativeFilledSpace;
  uint64_t cumulativeEvictedSpace;
	// number of requests to the cache 
  uint64_t accesses;
  uint64_t availableCapacity;
	// size of objects stored in the cache 
  uint64_t consumedCapacity;
    // no. of warm-up accesses 
    uint64_t warmupAccesses; 
	// no. of misses during warm-up 
	uint64_t warmupMisses; 
	// candidate_t{int appId; int64_t id;} 
	//	int64_t id is the object ID 
	// sizeMap stores key-value pairs. Value is size in uint32_t 
  std::unordered_map<repl::candidate_t, uint32_t> sizeMap;
  repl::CandidateMap<bool> historyAccess;

  Cache()
    : repl(nullptr)
    , hits(0)
    , misses(0)
    , compulsoryMisses(0)
    , fills(0)
    , evictions(0)
    , accessesTriggeringEvictions(0)
    , missesTriggeringEvictions(0)
    , cumulativeAllocatedSpace(0)
    , cumulativeFilledSpace(0)
    , cumulativeEvictedSpace(0)
    , accesses(0)
    , availableCapacity(-1)
    , consumedCapacity(0)
	, warmupMisses(0)
    , historyAccess(false) {}

  uint32_t getSize(repl::candidate_t id) const {
    auto itr = sizeMap.find(id);
    if (itr != sizeMap.end()) {
      return itr->second;
    } else {
      return -1u;
    }
  }

  uint32_t getNumObjects() const {
    return sizeMap.size();
  }

  void access(const parser::Request& req) {
    assert(req.size() > 0);
    if (req.type != parser::GET) { return; }

	// namespace repl 
	// repl::struct candidate_t{} defined in candidate.hpp 
	//	field: int appId, int64_t id
	// In candidate.hpp, 
	// struct candidate_t{ 
	//	static candidate_t make(const parser::Request& req) {...}
	// }
	// make(req) returns an object of struct candidate_t 
    auto id = repl::candidate_t::make(req); // datatype(id) is candidate_t  
	// struct Cache { std::unordered_map<repl::candidate_t, uint32_t> sizeMap; }
    auto itr = sizeMap.find(id);
    bool hit = (itr != sizeMap.end());

	// struct Cache{repl::CandidateMap<bool> historyAccess; } 
	//	In candidate.hpp, ...
	// 	class CandidateMap : public std::unordered_map<candidate_t, T> {} 
    if (!historyAccess[id]) {
      // first time requests are considered as compulsory misses
      ++compulsoryMisses;
      historyAccess[id] = true;
    }

    if (hit) { ++hits; } else { 
	    if(accesses < warmupAccesses) {
		    warmupMisses++;
	    }
	    ++misses; 
    }
    ++accesses;

    // stats?
    if ((STATS_INTERVAL > 0) && ((accesses % STATS_INTERVAL) == 0)) {
        std::cout << "Stats: "
                  << hits << ", "
                  << misses << ", "
                  << fills << ", "
                  << compulsoryMisses << ", "
                  << (100. * hits / accesses)
                  << std::endl;
    }

    uint32_t requestSize = req.size();
    if (requestSize >= availableCapacity) {
        std::cerr << "Request too big: " << requestSize << " > " << availableCapacity << std::endl;
    }
    assert(requestSize < availableCapacity);
    
    uint32_t cachedSize = 0;
    if (hit) {
      cachedSize = itr->second;
      consumedCapacity -= cachedSize;
    }

    uint32_t evictionsFromThisAccess = 0;
    uint64_t evictedSpaceFromThisAccess = 0;

    while (consumedCapacity + requestSize > availableCapacity) {
      // need to evict stuff!
	// repl::Policy* repl; 
	//	class Policy {
	//		virtual candidate_t rank(const parser::Request& req) = 0;
	//	}
	// When LHD in use, rank() implemented in 
	//	class LHD : public virtual Policy {
	//		candidate_t rank(const parser::Request& req);
	//	}
      repl::candidate_t victim = repl->rank(req);
      auto victimItr = sizeMap.find(victim);
      if (victimItr == sizeMap.end()) {
        std::cerr << "Couldn't find victim: " << victim << std::endl;
      }
      assert(victimItr != sizeMap.end());

      repl->replaced(victim);

      // replacing candidate that just hit; don't free space twice
      if (victim == id) {
        continue;
      }

      evictionsFromThisAccess += 1;
      evictedSpaceFromThisAccess += victimItr->second;
      consumedCapacity -= victimItr->second;
      sizeMap.erase(victimItr);
    }

    // indicate where first eviction happens
    if (evictionsFromThisAccess > 0) {
      ++accessesTriggeringEvictions;
      if (evictions == 0) { std::cout << "x"; }
    }

    // measure activity
    evictions += evictionsFromThisAccess;
    cumulativeEvictedSpace += evictedSpaceFromThisAccess;
    if (hit) {
      if (requestSize > cachedSize) {
        cumulativeAllocatedSpace += requestSize - cachedSize;
      }
    } else {
      cumulativeAllocatedSpace += requestSize;
      if (evictionsFromThisAccess == 0) {
        // misses that don't require evictions are fills by definition
        ++fills;
        cumulativeFilledSpace += requestSize;
      } else {
        ++missesTriggeringEvictions;
      }
    }

    // insert request
    sizeMap[id] = requestSize;
    consumedCapacity += requestSize;

    assert(consumedCapacity <= availableCapacity);
    repl->update(id, req);
  }

  void dumpStats() {
    using std::endl;
    std::cout 
      << "Accesses: " << accesses
      << "\t(" << misc::bytes(cumulativeAllocatedSpace) << ")" << endl
      << "Hits: " << hits << " " << (100. * hits / accesses) << "%" << endl
      << "Misses: " << (misses-warmupMisses) << " " << (100. * (misses-warmupMisses) / (accesses-warmupAccesses)) << "%" << endl
      << "Compulsory misses: " << compulsoryMisses << " " << (100. * compulsoryMisses / accesses) << "%" << endl
      << "Non-compulsory hit rate: " << (100. * hits / (accesses - compulsoryMisses)) << "%" << endl
      << "  > Fills: " << fills << " " << (100. * fills / accesses) << "%"
      << "\t(" << misc::bytes(cumulativeFilledSpace) << ")" << endl
      << "  > Misses triggering evictions: " << missesTriggeringEvictions << " " << (100. * missesTriggeringEvictions / accesses) << "%" << endl
      << "  > Evictions: " << evictions << " " << (100. * evictions / accesses) << "%"
      << "\t(" << misc::bytes(cumulativeEvictedSpace) << ")" << endl
      << "  > Accesses triggering evictions: " << accessesTriggeringEvictions << " (" << (1. * evictions / accessesTriggeringEvictions) << " evictions per trigger)" << endl
        << "  > Warmup misses: " << warmupMisses << endl 
        << "  > Warmup accesses: " << warmupAccesses << endl 
      ;
  }

}; // struct Cache

}
