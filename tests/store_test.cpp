
#include "lldstore/store.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lldstore {
namespace {

TEST(Store, GetOnEmptyStore) {
    Store store;

    EXPECT_EQ(store.Size(), 0u);
    EXPECT_FALSE(store.Get("missing").has_value());
}

TEST(Store, PutThenGet) {
    Store store;

    store.Put("name", "Ratul");

    auto result = store.Get("name");

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Ratul");
    EXPECT_EQ(store.Size(), 1u);
}

TEST(Store, PutOverwritesExistingValue) {
    Store store;

    store.Put("name", "Ratul");
    store.Put("name", "Updated");

    auto result = store.Get("name");

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "Updated");
    EXPECT_EQ(store.Size(), 1u);
}

TEST(Store, DeleteExistingKey) {
    Store store;

    store.Put("name", "Ratul");

    EXPECT_TRUE(store.Delete("name"));
    EXPECT_FALSE(store.Get("name").has_value());
    EXPECT_EQ(store.Size(), 0u);
}

TEST(Store, DeleteMissingKey) {
    Store store;

    EXPECT_FALSE(store.Delete("missing"));
    EXPECT_EQ(store.Size(), 0u);
}

TEST(Store, EmptyKeyAndValue) {
    Store store;

    store.Put("", "");
    
    auto result = store.Get("");

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "");
    EXPECT_EQ(store.Size(), 1u);
}

TEST(Store, PrefixKeysAreDistinct) {
    Store store;

    store.Put("app", "one");
    store.Put("apple", "two");

    ASSERT_TRUE(store.Get("app").has_value());
    ASSERT_TRUE(store.Get("apple").has_value());

    EXPECT_EQ(*store.Get("app"), "one");
    EXPECT_EQ(*store.Get("apple"), "two");
    EXPECT_EQ(store.Size(), 2u);
}

TEST(Store, LargeValue) {
    Store store;

    std::string value(1'000'000, 'x');
    store.Put("large", value);

    auto result = store.Get("large");

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), value.size());
    EXPECT_EQ(*result, value);
}

TEST(Store, BinaryDataContainingNullCharacter) {
    Store store;

    std::string value("ab\0cd", 5);
    store.Put("binary", value);

    auto result = store.Get("binary");

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->size(), 5u);
    EXPECT_EQ(*result, std::string_view(value.data(), value.size()));
}

TEST(Store, BinaryKeysWithNullCharactersAreDistinct) {
    Store store;

    store.Put(std::string_view("a\0b", 3), "three");
    store.Put(std::string_view("a", 1), "one");
    store.Put(std::string_view("a\0", 2), "two");

    EXPECT_EQ(store.Size(), 3u);
    EXPECT_EQ(*store.Get(std::string_view("a\0b", 3)), "three");
    EXPECT_EQ(*store.Get(std::string_view("a", 1)), "one");
    EXPECT_EQ(*store.Get(std::string_view("a\0", 2)), "two");
}

TEST(Store, DeleteLeavesOtherKeysIntactAndKeyCanBeReinserted) {
    Store store;

    store.Put("a", "1");
    store.Put("b", "2");
    store.Put("c", "3");

    EXPECT_TRUE(store.Delete("b"));
    EXPECT_EQ(store.Size(), 2u);
    EXPECT_EQ(*store.Get("a"), "1");
    EXPECT_EQ(*store.Get("c"), "3");
    EXPECT_FALSE(store.Delete("b"));  // second delete of the same key misses

    store.Put("b", "new");
    EXPECT_EQ(store.Size(), 3u);
    EXPECT_EQ(*store.Get("b"), "new");
}

TEST(Store, OverwriteWithLongerAndShorterValues) {
    Store store;

    store.Put("k", "short");
    store.Put("k", std::string(5000, 'L'));
    EXPECT_EQ(*store.Get("k"), std::string(5000, 'L'));

    store.Put("k", "s");
    EXPECT_EQ(*store.Get("k"), "s");

    store.Put("k", std::string(300, 'M'));
    EXPECT_EQ(*store.Get("k"), std::string(300, 'M'));
    EXPECT_EQ(store.Size(), 1u);
}

// Enough keys to force several rehashes; every key must survive each one.
TEST(Store, ManyKeysSurviveRehashing) {
    Store store;
    constexpr int kKeys = 10000;

    for (int i = 0; i < kKeys; ++i) store.Put("key" + std::to_string(i), std::to_string(i * 7));
    ASSERT_EQ(store.Size(), static_cast<size_t>(kKeys));

    for (int i = 0; i < kKeys; ++i) {
        const auto got = store.Get("key" + std::to_string(i));
        ASSERT_TRUE(got.has_value()) << "i=" << i;
        ASSERT_EQ(*got, std::to_string(i * 7)) << "i=" << i;
    }

    for (int i = 0; i < kKeys; i += 2) ASSERT_TRUE(store.Delete("key" + std::to_string(i)));
    EXPECT_EQ(store.Size(), static_cast<size_t>(kKeys / 2));
    for (int i = 0; i < kKeys; ++i) {
        EXPECT_EQ(store.Get("key" + std::to_string(i)).has_value(), i % 2 == 1) << "i=" << i;
    }
}

// Random Put/Get/Delete against a reference std::unordered_map. A small key space makes keys
// collide, get deleted and get re-inserted often. The seed is fixed so a failure is reproducible;
// change kSeed to explore other sequences.
TEST(Store, RandomOperationsMatchReferenceModel) {
    constexpr uint64_t kSeed = 12345;
    constexpr int kSteps = 100000;
    constexpr uint64_t kKeySpace = 50;
    SCOPED_TRACE("seed=" + std::to_string(kSeed));

    std::mt19937_64 rng(kSeed);
    Store store;
    std::unordered_map<std::string, std::string> model;

    for (int step = 0; step < kSteps; ++step) {
        const std::string key = "k" + std::to_string(rng() % kKeySpace);
        switch (rng() % 3) {
            case 0: {
                std::string value(rng() % 200, '\0');
                for (char& c : value) c = static_cast<char>(rng() % 256);  // includes '\0'
                store.Put(key, value);
                model[key] = value;
                break;
            }
            case 1: {
                const auto got = store.Get(key);
                const auto expected = model.find(key);
                ASSERT_EQ(got.has_value(), expected != model.end()) << "step=" << step;
                if (got.has_value()) {  // braces: gtest macros contain an else (GCC -Wdangling-else)
                    ASSERT_EQ(*got, expected->second) << "step=" << step;
                }
                break;
            }
            default: {
                const bool erased = store.Delete(key);
                ASSERT_EQ(erased, model.erase(key) == 1) << "step=" << step;
                break;
            }
        }
        ASSERT_EQ(store.Size(), model.size()) << "step=" << step;
    }

    for (const auto& [key, value] : model) {
        const auto got = store.Get(key);
        ASSERT_TRUE(got.has_value()) << key;
        ASSERT_EQ(*got, value) << key;
    }
}

}  // namespace
}  // namespace lldstore
