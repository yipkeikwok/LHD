#pragma once

#include <stdint.h>

namespace cache {

const uint64_t STATS_INTERVAL = 1000000;
const uint64_t WARMUP_ACCESSES = 128*1000*1000;
}

namespace parser{

const uint64_t FAST_FORWARD = 0;

}
