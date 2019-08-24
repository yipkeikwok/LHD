#pragma once
#define WARMUP
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
	// no. of misses during warm-up 
	uint64_t warmupMisses; 
	// candidate_t{int appId; int64_t id;} 
	//	int64_t id is the object ID 
	// sizeMap stores key-value pairs. Value is size in uint32_t 
  std::unordered_map<repl::candidate_t, uint32_t> sizeMap;
    #ifdef LHD_LHD
  repl::CandidateMap<bool> victimSet;
    #endif 
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
    #ifdef LHD_LHD
    , victimSet(false) 
    #endif
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
    bool firstTimeAccessToObject = false; 

	// struct Cache{repl::CandidateMap<bool> historyAccess; } 
	//	In candidate.hpp, ...
	// 	class CandidateMap : public std::unordered_map<candidate_t, T> {} 
    if (!historyAccess[id]) {
        assert(!hit); // first-time requests must be misses 
      // first time requests are considered as compulsory misses
      ++compulsoryMisses;
      historyAccess[id] = true;
        firstTimeAccessToObject = true; 
    }

    if (hit) { ++hits; } else { 
	    if(accesses < WARMUP_ACCESSES) {
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

    #ifndef LHD_LHD
    while (consumedCapacity + requestSize > availableCapacity) {
        if(hit) {
            assert(cachedSize<requestSize);
           }
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

        // A hit object gets replaced (evicted) too but it is legit. 
        // Reason: The execution gets here when access is hit only because of 
        // object size change (cachedSize<requestSize). In this case, need to 
        // evict the smaller cached version (size=cacheSize) and admit the 
        // larger version (size=requestSize), which is being requested. 
      repl->replaced(victim);

      // replacing candidate that just hit; don't free space twice
      if (victim == id) {
        // It is possible (victim == id) because repl->rank() picks victim from 
        // std::vector<Tag> tags, which contains the Tag struct of all, and only, 
        // objects currently in the cache. 
        // However, this can happen only if the access is a hit. 
        assert(hit);
        continue;
      }

      evictionsFromThisAccess += 1;
      evictedSpaceFromThisAccess += victimItr->second;
      consumedCapacity -= victimItr->second;
      sizeMap.erase(victimItr);
    }
    #else
    // potential consumed capacity after {victims} are evicted 
    uint64_t consumedCapacity2LHD; 

    consumedCapacity2LHD = consumedCapacity; 
    victimSet.clear(); 
    assert(victimSet.size()==(size_t)0);
    uint32_t nrIteration0 = (uint32_t)0; 
    #ifdef DEBUG_UNRESOLVED20190817a 
    uint32_t alreadyInVictimSetCnt = (uint32_t)0; 
    #endif
    while(consumedCapacity2LHD + requestSize > availableCapacity) {
        if(hit) {
             assert(cachedSize<requestSize);
        }

        repl::candidate_t victim = repl->rank(req); 
        auto victimItr = sizeMap.find(victim); 
        if(victimItr==sizeMap.end()) {
            std::cerr << "Couldn't find victim in sizeMap: " << victim << std::endl;
        }
        assert(victimItr!=sizeMap.end()); 

        // WARNING: This may cause an infinite while loop if all cached object have 
        //  been picked as victims but consumedCapacity2LHD + requestSize is 
        //  still larger than availableCapacity 
        //  see <UNRESOLVED::20190817a> on Google doc 
        if(victimSet[victim]) {
            #ifdef DEBUG_UNRESOLVED20190817a 
            size_t nrVictimSetObject = victimSet.size(); 
            size_t nrCachedObject = repl->getNrCachedObject(); 
            std::cout<<"aIVSC= "<<++alreadyInVictimSetCnt;
            std::cout<<"; nVSO= "<<nrVictimSetObject; 
            std::cout<<"; nCO= "<<nrCachedObject;
            std::cout<<"; oID= "<<victim.id; 
            if(nrVictimSetObject==nrCachedObject) {
                    std::cout<<"; victimSet.size()==repl->getNrCachedObject()";
            }
            std::cout<< "" <<std::endl; 
            #endif
            continue;
        }
        victimSet[victim]=true;
        nrIteration0++; 

        if(victim!=id) {
            consumedCapacity2LHD-=victimItr->second; 
        }
    } // while(consumedCapacity2LHD + requestSize > availableCapacity) {
    assert(nrIteration0==victimSet.size()); 
    if(hit && !(cachedSize<requestSize)) {
        assert(victimSet.size()==0);
    }
    // TO_CONTINUE: handle the case when the victim selected is the requested 
    //  object 
    // TO_CONTINUE: handle the following statement from lhd.cpp::rank() 
    //  ewmaVictimHitDensity = EWMA_DECAY * ewmaVictimHitDensity + (1 - EWMA_DECAY) 
    //  * victimRank;

    bool evictDecision = false; 
    assert(!evictDecision); 
    if((victimSet.size() > 0) && !firstTimeAccessToObject) {
        evictDecision = repl->toEvict(id, requestSize, victimSet);
    }
    // CASE: LHD-LHD decides to evict {victimSet} 
    if(evictDecision || firstTimeAccessToObject) {
        uint32_t nrIteration1= (uint32_t)0; 
        for(const auto& victim : victimSet) {
         auto victimItr = sizeMap.find(victim.first);
         if (victimItr == sizeMap.end()) {
             std::cerr << "Couldn't find victim: " << victim.first << std::endl;
         }
         assert(victimItr != sizeMap.end());

         repl->replaced(victim.first); 
         nrIteration1++; 
         if(victim.first == id) {
             continue;
         }
         evictionsFromThisAccess += 1;
         evictedSpaceFromThisAccess += victimItr->second;
         consumedCapacity -= victimItr->second;
            sizeMap.erase(victimItr);
        }
       assert(nrIteration0==nrIteration1); 
    } // if(evictDecision) {
    #endif 

    // indicate where first eviction happens
    if (evictionsFromThisAccess > 0) {
      ++accessesTriggeringEvictions;
      if (evictions == 0) { std::cout << "x"; }
    }

    #ifndef LHD_LHD
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
    #else 
    // measure activity
    evictions += evictionsFromThisAccess;
    cumulativeEvictedSpace += evictedSpaceFromThisAccess;
    if (hit) {
        assert(!firstTimeAccessToObject); 
      if (requestSize > cachedSize) {
        if(evictDecision) {
            cumulativeAllocatedSpace += requestSize - cachedSize;
        } // if(evictDecision) {
      }
    } else {
        // MISS
        // if (victimSet.size()==0), admitting the requested object does not 
        // require evicting any cached objects 
        // CONTINUE_HERE::20190825
        if(evictDecision ) {
            assert(evictionsFromThisAccess > 0); 
           cumulativeAllocatedSpace += requestSize;
        }
        if(firstTimeAccessToObject) {
           cumulativeAllocatedSpace += requestSize;
        }
    
      if ((evictionsFromThisAccess == 0) && (victimSet.size()==0)) {
        // misses that don't require evictions are fills by definition
            ++fills;
            cumulativeFilledSpace += requestSize;
      } else if ((evictionsFromThisAccess>0) && (victimSet.size()>0)) {
        assert(evictDecision==true); 
        ++missesTriggeringEvictions;
      } else if ((evictionsFromThisAccess==0) && (victimSet.size()>0)) {
        assert(evictDecision==false);
      } else if ((evictionsFromThisAccess>0) && (victimSet.size()==0)) {
        std::cerr << "(evictionFromThisAccess>0) && (victimSet.size()==0)" << 
            " should not happend" << std::endl; 
        assert(0==1);
      } else {
        std::cerr << " should not happend" << std::endl; 
        assert(0==1);
      } 
    }
    #endif // #ifndef LHD_LHD

    // insert request
    #ifndef LHD_LHD 
    sizeMap[id] = requestSize;
    consumedCapacity += requestSize;

    #else 
    if(hit) {
        if(cachedSize<requestSize) {
            if(evictDecision && (victimSet.size()>0)) {
                assert(evictionsFromThisAccess>0); 
                sizeMap[id] = requestSize; 
                consumedCapacity += requestSize; 
            } else {
                assert(evictionsFromThisAccess==0); 
                consumedCapacity += cachedSize; 
            }
        } else {
            // cachedSize>=requestSize 
            assert(evictionsFromThisAccess==0); 
            sizeMap[id] = requestSize; 
            consumedCapacity += requestSize; 
        }
    } else {
        // MISS
        if(evictDecision || (victimSet.size()==0)) {
            // CASE: admit 
            if(!(evictDecision && (victimSet.size()==0))) {
                assert(firstTimeAccessToObject);
            }
            if(evictDecision) {
                if(!evictDecision) {
                    assert(evictionsFromThisAccess>0);
                }
            } else {
                assert(victimSet.size()>0);
            }
            sizeMap[id] = requestSize;
            consumedCapacity += requestSize; 
        } else {
            // CASE: not admit 
        }
    }
    #endif //#ifndef LHD_LHD
    if(consumedCapacity > availableCapacity) {
        assert(consumedCapacity <= availableCapacity);
    }
    repl->update(id, req);
  }

  void dumpStats() {
    using std::endl;
    std::cout 
      << "Accesses: " << accesses
      << "\t(" << misc::bytes(cumulativeAllocatedSpace) << ")" << endl
      << "Hits: " << hits << " " << (100. * hits / accesses) << "%" << endl
#ifndef	WARMUP
      << "Misses: " << (misses) << " " << (100. * (misses) / (accesses)) << "%" << endl
#else 
      << "Misses: " << (misses-warmupMisses) << " " << (100. * (misses-warmupMisses) / (accesses-WARMUP_ACCESSES)) << "%" << endl
#endif
      << "Compulsory misses: " << compulsoryMisses << " " << (100. * compulsoryMisses / accesses) << "%" << endl
      << "Non-compulsory hit rate: " << (100. * hits / (accesses - compulsoryMisses)) << "%" << endl
      << "  > Fills: " << fills << " " << (100. * fills / accesses) << "%"
      << "\t(" << misc::bytes(cumulativeFilledSpace) << ")" << endl
      << "  > Misses triggering evictions: " << missesTriggeringEvictions << " " << (100. * missesTriggeringEvictions / accesses) << "%" << endl
      << "  > Evictions: " << evictions << " " << (100. * evictions / accesses) << "%"
      << "\t(" << misc::bytes(cumulativeEvictedSpace) << ")" << endl
      << "  > Accesses triggering evictions: " << accessesTriggeringEvictions << " (" << (1. * evictions / accessesTriggeringEvictions) << " evictions per trigger)" << endl
      ;
  }

}; // struct Cache

}
