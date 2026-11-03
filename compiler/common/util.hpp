#pragma once

#include <cstdint>
#include <vector>

namespace parc {

// Container sizes as int, so loops and indices don't mix signed and unsigned.
template <typename C>
int isize(const C& container) {
    return static_cast<int>(container.size());
}

// A fixed-size set of small integers, used by the data-flow analyses
// (one bit per variable, definition or expression).
class BitSet {
public:
    BitSet() = default;
    explicit BitSet(int size, bool value = false)
        : size_(size), words_((static_cast<size_t>(size) + 63) / 64, value ? ~0ULL : 0ULL) {
        trim();
    }

    int size() const { return size_; }
    bool test(int i) const { return (words_[static_cast<size_t>(i) / 64] >> (i % 64)) & 1ULL; }
    void set(int i) { words_[static_cast<size_t>(i) / 64] |= 1ULL << (i % 64); }
    void reset(int i) { words_[static_cast<size_t>(i) / 64] &= ~(1ULL << (i % 64)); }

    void unite(const BitSet& other) {
        for (size_t w = 0; w < words_.size(); ++w) words_[w] |= other.words_[w];
    }
    void intersect(const BitSet& other) {
        for (size_t w = 0; w < words_.size(); ++w) words_[w] &= other.words_[w];
    }
    void subtract(const BitSet& other) {
        for (size_t w = 0; w < words_.size(); ++w) words_[w] &= ~other.words_[w];
    }

    bool operator==(const BitSet& other) const = default;

    std::vector<int> members() const {
        std::vector<int> out;
        for (int i = 0; i < size_; ++i)
            if (test(i)) out.push_back(i);
        return out;
    }

private:
    int size_ = 0;
    std::vector<std::uint64_t> words_;

    void trim() {
        if (size_ % 64 != 0 && !words_.empty()) words_.back() &= (1ULL << (size_ % 64)) - 1;
    }
};

}  // namespace parc
