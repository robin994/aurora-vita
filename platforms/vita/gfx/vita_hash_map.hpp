#pragma once
#include <functional>
#include <robin_hood.h>

namespace aurora::vita::gfx {

template <typename Key, typename Value, typename Hash = robin_hood::hash<Key>,
          typename Equal = std::equal_to<Key>>
using FlatHashMap = robin_hood::unordered_flat_map<Key, Value, Hash, Equal>;

template <typename Key, typename Value, typename Hash = robin_hood::hash<Key>,
          typename Equal = std::equal_to<Key>>
using NodeHashMap = robin_hood::unordered_node_map<Key, Value, Hash, Equal>;

template <typename Key, typename Hash = robin_hood::hash<Key>, typename Equal = std::equal_to<Key>>
using FlatHashSet = robin_hood::unordered_flat_set<Key, Hash, Equal>;

} // namespace aurora::vita::gfx
