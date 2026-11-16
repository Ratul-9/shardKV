#include "lldstore/flat_store.h"
#include "lldstore/store.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>

#define Store FlatStore
#include "store_test.cpp"
