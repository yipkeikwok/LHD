#include <sstream>
#include "cache.hpp"
#include "lhd.hpp"
#include "rand.hpp"

namespace repl {

// invoked by 
//	repl::Policy* repl::Policy::create(cache::Cache* cache, 
//		const libconfig::Setting &settings)
//	in repl.cpp 
//	_associativity is from cache={assoc} in example.cfg 
//	_admissions is from cache={admissionSamples} in example.cfg 
LHD::LHD(int _associativity, int _admissions, cache::Cache* _cache)
    : ASSOCIATIVITY(_associativity)
    , ADMISSIONS(_admissions)
    , cache(_cache)
    , recentlyAdmitted(ADMISSIONS, INVALID_CANDIDATE) {
    nextReconfiguration = ACCS_PER_RECONFIGURATION;
    explorerBudget = _cache->availableCapacity * EXPLORER_BUDGET_FRACTION;
 
	// lhd.hpp:    
	//	static constexpr uint32_t NUM_CLASSES = HIT_AGE_CLASSES 
	//		* APP_CLASSES;   
    for (uint32_t i = 0; i < NUM_CLASSES; i++) {
        classes.push_back(Class());
        auto& cl = classes.back();
	// lhd.hpp:    
	//	static constexpr age_t MAX_AGE = 20000;
	//	Note: MAX_AGE is a coarsened age 
        cl.hits.resize(MAX_AGE, 0);
        cl.evictions.resize(MAX_AGE, 0);
        cl.hitDensities.resize(MAX_AGE, 0);
    }

    // Initialize policy to ~GDSF by default.
    for (uint32_t c = 0; c < NUM_CLASSES; c++) {
        for (age_t a = 0; a < MAX_AGE; a++) {
            classes[c].hitDensities[a] =
                1. * (c + 1) / (a + 1);
        }
    }
}

// return struct candidate_t of the eviction victim 
candidate_t LHD::rank(const parser::Request& req) {
    uint64_t victim = -1;
	// lhd.hpp
	//	namespace repl {
	//		class LHD : public virtual Policy {
	//			private: 
	//				typedef float rank_t; 
	//		}
	//	}
    rank_t victimRank = std::numeric_limits<rank_t>::max();

    // Sample few candidates early in the trace so that we converge
    // quickly to a reasonable policy.
    //
    // This is a hack to let us have shorter warmup so we can evaluate
    // a longer fraction of the trace; doesn't belong in a real
    // system.
    uint32_t candidates =
        (numReconfigurations > 50)?
        ASSOCIATIVITY : 8;

    for (uint32_t i = 0; i < candidates; i++) {
	// lhd.hpp::namespace repl::class LHD 
	//	std::vector<Tag> tags; 
	//	struct Tag {
	//		age_t timestamp;
	//		age_t lastHitAge;
	//		age_t lastLastHitAge;
	//		uint32_t app;

	//		candidate_t id;
	//		rank_t size; // stored redundantly with cache
	//		bool explorer;
	//	};
	//	typedef uint64_t age_t; 
	//	typedef float rank_t; 
        auto idx = rand.next() % tags.size();
        auto& tag = tags[idx];
	// lhd.hpp
	//	inline rank_t getHitDensity(const Tag& tag) {}
        rank_t rank = getHitDensity(tag);

        if (rank < victimRank) {
            victim = idx;
            victimRank = rank;
        }
    }

    for (uint32_t i = 0; i < ADMISSIONS; i++) {
	// lhd.hpp::namespace repl::class LHD::
	//	std::unordered_map<candidate_t, uint64_t> indices;
        auto itr = indices.find(recentlyAdmitted[i]);
	// a recently admitted may have already been evicted and, therefore, not 
	//	in indices 
        if (itr == indices.end()) { continue; }

        auto idx = itr->second;
        auto& tag = tags[idx];
        assert(tag.id == recentlyAdmitted[i]);
        rank_t rank = getHitDensity(tag);

        if (rank < victimRank) {
            victim = idx;
            victimRank = rank;
        }
    }

    assert(victim != (uint64_t)-1);

	// lhd.hpp::namespace repl::class LHD::
	//	rank_t ewmaVictimHitDensity = 0;
	//	typedef float rank_t;
    ewmaVictimHitDensity = EWMA_DECAY * ewmaVictimHitDensity + (1 - EWMA_DECAY) * victimRank;

    return tags[victim].id;
}

// called by namespace cache::class Cache::access() 
void LHD::update(candidate_t id, const parser::Request& req) {
    auto itr = indices.find(id);
    bool insert = (itr == indices.end());
        
    Tag* tag;
    if (insert) {
	// namespace cache{class Cache{std::vector<Tag> tags;}}
        tags.push_back(Tag{});
	// back(): returns reference to the last element 
        tag = &tags.back();
        indices[id] = tags.size() - 1;
        
        tag->lastLastHitAge = MAX_AGE;
        tag->lastHitAge = 0;
        tag->id = id;
    } else {
        tag = &tags[itr->second];
        assert(tag->id == id);
	// lhd.hpp
	//	inline age_t getAge(Tag tag) {...} 
	//	returns coarsened age 
        auto age = getAge(*tag);
	// lhd.hpp 
	//	inline Class& getClass(const Tag& tag) {...} 
        auto& cl = getClass(*tag);
        cl.hits[age] += 1;

        if (tag->explorer) { explorerBudget += tag->size; }
        
        tag->lastLastHitAge = tag->lastHitAge;
        tag->lastHitAge = age;
    }

    tag->timestamp = timestamp;
    tag->app = req.appId % APP_CLASSES;
    tag->size = req.size();

    // with some probability, some candidates will never be evicted
    // ... but limit how many resources we spend on doing this
    bool explore = (rand.next() % EXPLORE_INVERSE_PROBABILITY) == 0;
    if (explore && explorerBudget > 0 && numReconfigurations < 50) {
        tag->explorer = true;
        explorerBudget -= tag->size;
    } else {
        tag->explorer = false;
    }

    // If this candidate looks like something that should be
    // evicted, track it.
    if (insert && !explore && getHitDensity(*tag) < ewmaVictimHitDensity) {
        recentlyAdmitted[recentlyAdmittedHead++ % ADMISSIONS] = id;
    }
    
    ++timestamp;

    if (--nextReconfiguration == 0) {
        reconfigure();
        nextReconfiguration = ACCS_PER_RECONFIGURATION;
        ++numReconfigurations;
    }
}

// invoked by cache.hpp
//	cache::struct Cache{void access(const parser::Request& req) {...}} 
void LHD::replaced(candidate_t id) {
	// lhd.hpp 
	// namespace repl { class LHD { 
		// stores the index of each candidate_t struct 
		//	in std::vector<Tag> tags
		// std::unordered_map<candidate_t, uint64_t> indices;
	// }	}
    auto itr = indices.find(id);
    assert(itr != indices.end());
    auto index = itr->second;

    // Record stats before removing item
    auto& tag = tags[index];
    assert(tag.id == id);
    auto age = getAge(tag);
    auto& cl = getClass(tag);
    cl.evictions[age] += 1;

    if (tag.explorer) { explorerBudget += tag.size; }

    // Remove tag for replaced item and update index
    indices.erase(itr);
    tags[index] = tags.back();
    tags.pop_back();

    if (index < tags.size()) {
        indices[tags[index].id] = index;
    }
}

void LHD::reconfigure() {
	// typedef float rank_t;
    rank_t totalHits = 0;
    rank_t totalEvictions = 0;
    for (auto& cl : classes) {
	// decay hits[] and evictions[] counts of each class 
	// sum up decayed hits[] and evictions[] counts 
	// 	cl.totalHits <- sum(cl.hits[]) 
	// 	cl.totalEvictions <- sum(cl.evictions[]) 
        updateClass(cl); 
        totalHits += cl.totalHits;
        totalEvictions += cl.totalEvictions;
    }

    adaptAgeCoarsening();
        
    modelHitDensity();

    // Just printfs ...
    for (uint32_t c = 0; c < classes.size(); c++) {
        auto& cl = classes[c];
        // printf("Class %d | hits %g, evictions %g, hitRate %g\n",
        //        c,
        //        cl.totalHits, cl.totalEvictions,
        //        cl.totalHits / (cl.totalHits + cl.totalEvictions));

        dumpClassRanks(cl);
    }
    printf("LHD | hits %g, evictions %g, hitRate %g | overflows %lu (%g) | cumulativeHitRate nan\n",
           totalHits, totalEvictions,
           totalHits / (totalHits + totalEvictions),
           overflows,
           1. * overflows / ACCS_PER_RECONFIGURATION);

    overflows = 0;
}

void LHD::updateClass(Class& cl) {
    cl.totalHits = 0;
    cl.totalEvictions = 0;

    for (age_t age = 0; age < MAX_AGE; age++) {
        cl.hits[age] *= EWMA_DECAY;
        cl.evictions[age] *= EWMA_DECAY;

        cl.totalHits += cl.hits[age];
        cl.totalEvictions += cl.evictions[age];
    }
}

// invoked by reconfigure() 
void LHD::modelHitDensity() {
	// ./lhd.hpp:    
	//	std::vector<Class> classes;
	//	number of elements = NUM_CLASSES = HIT_AGE_CLASSES*APP_CLASSES 
    for (uint32_t c = 0; c < classes.size(); c++) {
        rank_t totalEvents = classes[c].hits[MAX_AGE-1] + classes[c].evictions[MAX_AGE-1];
        rank_t totalHits = classes[c].hits[MAX_AGE-1];
        rank_t lifetimeUnconditioned = totalEvents;

        // we use a small trick here to compute expectation in O(N) by
        // accumulating all values at later ages in
        // lifetimeUnconditioned.
 
	// How does this for-loop end? 
	//	my guess: Since typedef uint64_t age_t, so age_t a keeps being 
	//	decremented, it becomes MAX(uint64_t) due to overflow and breaks 
	//	the loop condition        
        for (age_t a = MAX_AGE - 2; a < MAX_AGE; a--) {
            totalHits += classes[c].hits[a];
            
            totalEvents += classes[c].hits[a] + classes[c].evictions[a];

            lifetimeUnconditioned += totalEvents;

            if (totalEvents > 1e-5) {
                classes[c].hitDensities[a] = totalHits / lifetimeUnconditioned;
            } else {
                classes[c].hitDensities[a] = 0.;
            }
        }
    }
}

void LHD::dumpClassRanks(Class& cl) {
    if (!DUMP_RANKS) { return; }
    
    // float objectAvgSize = cl.sizeAccumulator / cl.totalHits; // + cl.totalEvictions);
    float objectAvgSize = 1. * cache->consumedCapacity / cache->getNumObjects();
    rank_t left;

    left = cl.totalHits + cl.totalEvictions;
    std::cout << "Ranks for avg object (" << objectAvgSize << "): ";
    for (age_t a = 0; a < MAX_AGE; a++) {
      std::stringstream rankStr;
      rank_t density = cl.hitDensities[a] / objectAvgSize;
      rankStr << density << ", ";
      std::cout << rankStr.str();

      left -= cl.hits[a] + cl.evictions[a];
      if (rankStr.str() == "0, " && left < 1e-2) {
        break;
      }
    }
    std::cout << std::endl;

    left = cl.totalHits + cl.totalEvictions;
    std::cout << "Hits: ";
    for (uint32_t a = 0; a < MAX_AGE; a++) {
      std::stringstream rankStr;
      rankStr << cl.hits[a] << ", ";
      std::cout << rankStr.str();

      left -= cl.hits[a] + cl.evictions[a];
      if (rankStr.str() == "0, " && left < 1e-2) {
        break;
      }
    }
    std::cout << std::endl;

    left = cl.totalHits + cl.totalEvictions;
    std::cout << "Evictions: ";
    for (uint32_t a = 0; a < MAX_AGE; a++) {
      std::stringstream rankStr;
      rankStr << cl.evictions[a] << ", ";
      std::cout << rankStr.str();

      left -= cl.hits[a] + cl.evictions[a];
      if (rankStr.str() == "0, " && left < 1e-2) {
        break;
      }
    }
    std::cout << std::endl;
}

// this happens very rarely!
//
// it is simple enough to set the age coarsening if you know roughly
// how big your objects are. to make LHD run on different traces
// without needing to configure this, we set the age coarsening
// automatically near the beginning of the trace.
void LHD::adaptAgeCoarsening() {
    ewmaNumObjects *= EWMA_DECAY;
    ewmaNumObjectsMass *= EWMA_DECAY;

    ewmaNumObjects += cache->getNumObjects();
    ewmaNumObjectsMass += 1.;

    rank_t numObjects = ewmaNumObjects / ewmaNumObjectsMass;

    rank_t optimalAgeCoarsening = 1. * numObjects / (AGE_COARSENING_ERROR_TOLERANCE * MAX_AGE);

    // Simplify. Just do this once shortly after the trace starts and
    // again after 25 iterations. It only matters that we are within
    // the right order of magnitude to avoid tons of overflows.
    if (numReconfigurations == 5 || numReconfigurations == 25) {
        uint32_t optimalAgeCoarseningLog2 = 1;

        while ((1 << optimalAgeCoarseningLog2) < optimalAgeCoarsening) {
            optimalAgeCoarseningLog2 += 1;
        }

        int32_t delta = optimalAgeCoarseningLog2 - ageCoarseningShift;
        ageCoarseningShift = optimalAgeCoarseningLog2;
        
        // increase weight to delay another shift for a while
        ewmaNumObjects *= 8;
        ewmaNumObjectsMass *= 8;
        
        // compress or stretch distributions to approximate new scaling
        // regime
        if (delta < 0) {
            // stretch
            for (auto& cl : classes) {
                for (age_t a = MAX_AGE >> (-delta); a < MAX_AGE - 1; a++) {
                    cl.hits[MAX_AGE - 1] += cl.hits[a];
                    cl.evictions[MAX_AGE - 1] += cl.evictions[a];
                }
                for (age_t a = MAX_AGE - 2; a < MAX_AGE; a--) {
                    cl.hits[a] = cl.hits[a >> (-delta)] / (1 << (-delta));
                    cl.evictions[a] = cl.evictions[a >> (-delta)] / (1 << (-delta));
                }
            }
        } else if (delta > 0) {
            // compress
            for (auto& cl : classes) {
                for (age_t a = 0; a < MAX_AGE >> delta; a++) {
                    cl.hits[a] = cl.hits[a << delta];
                    cl.evictions[a] = cl.evictions[a << delta];
                    for (int i = 1; i < (1 << delta); i++) {
                        cl.hits[a] += cl.hits[(a << delta) + i];
                        cl.evictions[a] += cl.evictions[(a << delta) + i];
                    }
                }
                for (age_t a = (MAX_AGE >> delta); a < MAX_AGE - 1; a++) {
                    cl.hits[a] = 0;
                    cl.evictions[a] = 0;
                }
            }
        }
    }
    
    printf("LHD at %lu | ageCoarseningShift now %lu | num objects %g | optimal age coarsening %g | current age coarsening %g\n",
           timestamp, ageCoarseningShift,
           numObjects,
           optimalAgeCoarsening,
           1. * (1 << ageCoarseningShift));
}

} // namespace repl
