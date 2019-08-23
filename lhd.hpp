#pragma once

#include <vector>
#include <limits>
#include "repl.hpp"
#include "rand.hpp"

namespace cache {

class Cache;

}

namespace repl {

class LHD : public virtual Policy {
  public:

    LHD(int _associativity, int _admissions, cache::Cache *cache);
    ~LHD() {}

    // called whenever and object is referenced
    void update(candidate_t id, const parser::Request& req);

    // called when an object is evicted
    void replaced(candidate_t id);

    // called to find a victim upon a cache miss
    candidate_t rank(const parser::Request& req);

    size_t getNrCachedObject(); 

    void dumpStats(cache::Cache* cache) { }

    #ifdef LHD_LHD
    bool toEvict(repl::candidate_t rqstd, repl::CandidateMap<bool>& victimSet); 
    #endif

  private:
    // TYPES ///////////////////////////////
    typedef uint64_t timestamp_t;
    typedef uint64_t age_t;
    typedef float rank_t;

    // info we track about each object
    struct Tag {
	// the un-coarsened time of the beginning of the block (single object's 
	//	lifetime) 
        age_t timestamp;
        age_t lastHitAge;
        age_t lastLastHitAge;
	// not actual appId
	// LHD::update(candidate_t id, const parser::Request& req) {
	//	tag->app = req.appId % APP_CLASSES;
	// } 
        uint32_t app;
        
        candidate_t id;
        rank_t size; // stored redundantly with cache
        bool explorer;
    };

    // info we track about each class of objects
    struct Class {
        std::vector<rank_t> hits;
        std::vector<rank_t> evictions;
        rank_t totalHits = 0;
        rank_t totalEvictions = 0;

        // vector hitDensities has not taken object size into account 
        // i.e., real HitDensity = hitDensities/(object size)
        std::vector<rank_t> hitDensities;
    };

    // CONSTANTS ///////////////////////////

    // how to sample candidates; can significantly impact hit
    // ratio. want a value at least 32; diminishing returns after
    // that.
    const uint32_t ASSOCIATIVITY = 32;

    // since our cache simulator doesn't bypass objects, we always
    // consider the last ADMISSIONS objects as eviction candidates
    // (this is important to avoid very large objects polluting the
    // cache.) alternatively, you could do bypassing and randomly
    // admit objects as "explorers" (see below).
    const uint32_t ADMISSIONS = 8;

    // escape local minima by having some small fraction of cache
    // space allocated to objects that aren't evicted. 1% seems to be
    // a good value that has limited impact on hit ratio while quickly
    // discovering the shape of the access pattern.
    static constexpr rank_t EXPLORER_BUDGET_FRACTION = 0.01;
	// if "explorer" space budget has not been reached, in lhd.cpp::update(), 
	// objects are being randomly put into the "explorer" space. The 
	// probability is 1/EXPLORE_INVERSE_PROBABILITY 
    static constexpr uint32_t EXPLORE_INVERSE_PROBABILITY = 32;

    // these parameters determine how aggressively to classify objects.
    // diminishing returns after a few classes; 16 is safe.
    static constexpr uint32_t HIT_AGE_CLASSES = 16;
    static constexpr uint32_t APP_CLASSES = 16;
    static constexpr uint32_t NUM_CLASSES = HIT_AGE_CLASSES * APP_CLASSES;
    
    // these parameters are tuned for simulation performance, and hit
    // ratio is insensitive to them at reasonable values (like these)
    static constexpr rank_t AGE_COARSENING_ERROR_TOLERANCE = 0.01;
    static constexpr age_t MAX_AGE = 20000;
    static constexpr timestamp_t ACCS_PER_RECONFIGURATION = (1 << 20);
    static constexpr rank_t EWMA_DECAY = 0.9;

    // verbose debugging output?
    static constexpr bool DUMP_RANKS = false;

    // FIELDS //////////////////////////////
    cache::Cache *cache;

    // object metadata; indices maps object id -> metadata
	// tags stores all cached objects 
    std::vector<Tag> tags;
    std::vector<Class> classes;
	// stores the index of each candidate_t struct in std::vector<Tag> tags 
    std::unordered_map<candidate_t, uint64_t> indices;
    // stores the hit density at age 0 when being evicted 
    // for DSGN20190731-0 
    std::unordered_map<candidate_t, rank_t> age0HitDensities;

    // time is measured in # of requests
    timestamp_t timestamp = 0;
    
    timestamp_t nextReconfiguration = 0;
    int numReconfigurations = 0;
    
    // how much to shift down age values; initial value doesn't really
    // matter, but must be positive. tuned in adaptAgeCoarsening() at
    // the beginning of the trace using ewmaNumObjects.
    timestamp_t ageCoarseningShift = 10;
    rank_t ewmaNumObjects = 0;
    rank_t ewmaNumObjectsMass = 0.;

    // how many objects had age > max age (this should almost never
    // happen -- if you observe non-neglible overflows, something has
    // gone wrong with the age coarsening!!!)
    uint64_t overflows = 0;

    misc::Rand rand;

    // see ADMISSIONS above
    std::vector<candidate_t> recentlyAdmitted;
    int recentlyAdmittedHead = 0;
	// used in LHD::rank() to identify newly admitted objects that should be 
	//	considered for upcoming evictions. In LHD::update(), the struct 
	//	candidate_t objects of 
	//	these objects are stored in recentlyAdmitted[]. 
	//	We do not want them to stay in the cache for too long because 
	//	their low densities (i.e., below ewmaVictimHitDensity) 
    rank_t ewmaVictimHitDensity = 0;

	// the amount of space allocated for "explorer" objects 
	// namespace repl { 
	//	LHD::LHD() {
	//		explorerBudget = _cache->availableCapacity 
	//			* EXPLORER_BUDGET_FRACTION;
	//
    int64_t explorerBudget = 0;
    
    // METHODS /////////////////////////////

    // returns something like log(maxAge - age)
    inline uint32_t hitAgeClass(age_t age) const {
        if (age == 0) { return HIT_AGE_CLASSES - 1; }
        uint32_t log = 0;
        while (age < MAX_AGE && log < HIT_AGE_CLASSES - 1) {
            age <<= 1;
            log += 1;
        }
        return log;
    }

    inline uint32_t getClassId(const Tag& tag) const {
        uint32_t hitAgeId = hitAgeClass(tag.lastHitAge + tag.lastLastHitAge);
	// Note: tag.app <- req.appId % APP_CLASSES 
        return tag.app * HIT_AGE_CLASSES + hitAgeId;
    }
    
/**
namespace cache {
	class LHD {
		struct Class {
			std::vector<rank_t> hits;
			std::vector<rank_t> evictions;
			rank_t totalHits = 0;
			rank_t totalEvictions = 0;

			std::vector<rank_t> hitDensities;
		};
	}
}
*/
    inline Class& getClass(const Tag& tag) {
        return classes[getClassId(tag)];
    }

	// return the coarsened age 
    inline age_t getAge(Tag tag) {
        timestamp_t age = (timestamp - (timestamp_t)tag.timestamp) >> ageCoarseningShift;

        if (age >= MAX_AGE) {
            ++overflows;
            return MAX_AGE - 1;
        } else {
            return (age_t) age;
        }
    }

    inline rank_t getHitDensity(const Tag& tag) {
	// age_t getAge(Tag tag) returns the coarsened age 
        auto age = getAge(tag);
        if (age == MAX_AGE-1) { return std::numeric_limits<rank_t>::lowest(); }
	// Class& getClass(const Tag& tag) {} 
        auto& cl = getClass(tag);
        rank_t density = cl.hitDensities[age] / tag.size;
        if (tag.explorer) { density += 1.; }
        return density;
    }
        
    void reconfigure();
    void adaptAgeCoarsening();
    void updateClass(Class& cl);
    void modelHitDensity();
    void dumpClassRanks(Class& cl);
};

} // namespace repl
