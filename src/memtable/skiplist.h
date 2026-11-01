#ifndef STRATA_MEMTABLE_SKIPLIST_H_
#define STRATA_MEMTABLE_SKIPLIST_H_

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>

#include "util/arena.h"
#include "util/check.h"
#include "util/random.h"

namespace strata {

// Lock-free skip list for StrataDB memtable.
//
// Thread safety / Rules for callers:
// 1. One writer at a time. In StrataDB that’s the write leader, enforced outside
//    the skiplist.
// 2. Readers need no lock but must keep the skiplist and its arena alive.
//    Thursday’s memtable will be reference-counted for exactly this.
// 3. Nothing is freed until the whole skiplist is destroyed.
//
// Memory ordering:
// - Writer allocates a new node and sets all its outgoing next pointers using
//   relaxed stores (NoBarrier_SetNext), since the node is not yet visible to readers.
// - Writer links the node into the skiplist from the bottom level up (level 0 to
//   height-1). Each incoming pointer is updated via SetNext with release memory order.
//   This release store synchronizes-with acquire loads on readers (Next), ensuring
//   that any reader seeing the new node also sees its key and its outgoing pointers.
// - Inserting bottom-up ensures that if a reader arrives at node x on level i,
//   all of x's pointers for levels 0..i are already valid.
// - Reading max_height_ with relaxed memory order is safe because a reader seeing
//   a new height before nodes are linked simply sees head_->Next(i) == nullptr and
//   drops down, while a reader seeing an older height simply bypasses higher levels.
template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {
      STRATA_DCHECK(list_ != nullptr);
    }

    // Returns true iff the iterator is positioned at a valid node.
    bool Valid() const { return node_ != nullptr; }

    // Returns the key at the current position.
    // REQUIRES: Valid()
    const Key& key() const {
      STRATA_DCHECK(Valid());
      return node_->key;
    }

    // Advances to the next position.
    // REQUIRES: Valid()
    void Next() {
      STRATA_DCHECK(Valid());
      node_ = node_->Next(0);
    }

    // Advances to the previous position.
    // REQUIRES: Valid()
    void Prev() {
      STRATA_DCHECK(Valid());
      node_ = list_->FindLessThan(node_->key);
    }

    // Advance to the first entry with a key >= target.
    void Seek(const Key& target) { node_ = list_->FindGreaterOrEqual(target, nullptr); }

    // Position at the first entry in list.
    // Final state of iterator is Valid() iff list is not empty.
    void SeekToFirst() { node_ = list_->head_->Next(0); }

    // Position at the last entry in list.
    // Final state of iterator is Valid() iff list is not empty.
    void SeekToLast() { node_ = list_->FindLast(); }

   private:
    const SkipList* list_;
    Node* node_;
  };

  // The arena must outlive the skiplist; nodes are never freed individually.
  explicit SkipList(Comparator cmp, Arena* arena);
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // REQUIRES: no key equal to `key` is already in the list.
  // REQUIRES: external synchronisation, so only one writer at a time.
  void Insert(const Key& key);

  // Returns true iff an entry that compares equal to key is in the list.
  // Safe to call concurrently with a writer inserting.
  bool Contains(const Key& key) const;

 private:
  static constexpr int kMaxHeight = 12;
  static constexpr int kBranching = 4;

  struct Node {
    explicit Node(const Key& k) : key(k), next_{nullptr} {}
    Key const key;

    Node* Next(int n) const {
      STRATA_DCHECK(n >= 0);
      STRATA_DCHECK(n < kMaxHeight);
      return next_[n].load(std::memory_order_acquire);
    }

    void SetNext(int n, Node* x) {
      STRATA_DCHECK(n >= 0);
      STRATA_DCHECK(n < kMaxHeight);
      next_[n].store(x, std::memory_order_release);
    }

    Node* NoBarrier_Next(int n) const {
      STRATA_DCHECK(n >= 0);
      STRATA_DCHECK(n < kMaxHeight);
      return next_[n].load(std::memory_order_relaxed);
    }

    void NoBarrier_SetNext(int n, Node* x) {
      STRATA_DCHECK(n >= 0);
      STRATA_DCHECK(n < kMaxHeight);
      next_[n].store(x, std::memory_order_relaxed);
    }

   private:
    friend class SkipList;
    // Really `height` entries allocated past the end of the struct.
    std::atomic<Node*> next_[1];
  };

  static Arena* CheckNotNull(Arena* arena) {
    STRATA_DCHECK(arena != nullptr);
    return arena;
  }

  Node* NewNode(const Key& key, int height);
  int RandomHeight();
  int GetMaxHeight() const { return max_height_.load(std::memory_order_relaxed); }

  bool Equal(const Key& a, const Key& b) const { return compare_(a, b) == 0; }

  // Returns true if n is not null and n->key is strictly less than key.
  bool KeyIsAfterNode(const Key& key, Node* n) const {
    return (n != nullptr) && (compare_(n->key, key) < 0);
  }

  // Returns the earliest node that comes at or after key.
  // If prev != nullptr, fills prev[level] with the last node before key at each level.
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const;

  // Returns the latest node with a key < key.
  // Returns nullptr if no such node exists.
  Node* FindLessThan(const Key& key) const;

  // Returns the last node in the list, or nullptr if list is empty.
  Node* FindLast() const;

  Comparator const compare_;
  Arena* const arena_;
  Node* const head_;
  std::atomic<int> max_height_;
  Random rnd_;
};

template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp),
      arena_(CheckNotNull(arena)),
      head_(NewNode(Key{}, kMaxHeight)),
      max_height_(1),
      rnd_(0xdeadbeef) {
  for (int i = 0; i < kMaxHeight; ++i) {
    head_->SetNext(i, nullptr);
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(const Key& key,
                                                                             int height) {
  STRATA_DCHECK(arena_ != nullptr);
  STRATA_DCHECK(height >= 1);
  STRATA_DCHECK(height <= kMaxHeight);
  char* const mem =
      arena_->AllocateAligned(sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
  Node* const node = new (mem) Node(key);
  for (int i = 1; i < height; ++i) {
    new (&node->next_[i]) std::atomic<Node*>(nullptr);
  }
  return node;
}

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
  int height = 1;
  while (height < kMaxHeight && rnd_.OneIn(kBranching)) {
    height++;
  }
  STRATA_DCHECK(height >= 1);
  STRATA_DCHECK(height <= kMaxHeight);
  return height;
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  Node* prev[kMaxHeight] = {};
  Node* x = FindGreaterOrEqual(key, prev);

  // Memtable keys are always unique; caller must guarantee key is absent.
  STRATA_DCHECK(x == nullptr || !Equal(key, x->key));

  int height = RandomHeight();
  int max_height = GetMaxHeight();
  if (height > max_height) {
    for (int i = max_height; i < height; ++i) {
      prev[i] = head_;
    }
    // Safe to store relaxed: readers seeing the new height before pointers are linked
    // will see head_->Next(i) == nullptr and drop down safely.
    max_height_.store(height, std::memory_order_relaxed);
  }

  x = NewNode(key, height);
  for (int i = 0; i < height; ++i) {
    // 1. Point x at its successor (relaxed: x is not yet published).
    x->NoBarrier_SetNext(i, prev[i]->NoBarrier_Next(i));
    // 2. Publish x (release: pairs with acquire on readers).
    prev[i]->SetNext(i, x);
  }
}

template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  Node* x = FindGreaterOrEqual(key, nullptr);
  if (x != nullptr && Equal(key, x->key)) {
    return true;
  }
  return false;
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindGreaterOrEqual(
    const Key& key, Node** prev) const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (KeyIsAfterNode(key, next)) {
      // Keep moving right on this level.
      x = next;
    } else {
      if (prev != nullptr) {
        prev[level] = x;
      }
      if (level == 0) {
        return next;
      }
      level--;
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLessThan(
    const Key& key) const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    STRATA_DCHECK(x == head_ || compare_(x->key, key) < 0);
    Node* next = x->Next(level);
    if (next == nullptr || compare_(next->key, key) >= 0) {
      if (level == 0) {
        return (x == head_) ? nullptr : x;
      }
      level--;
    } else {
      x = next;
    }
  }
}

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLast() const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (next == nullptr) {
      if (level == 0) {
        return (x == head_) ? nullptr : x;
      }
      level--;
    } else {
      x = next;
    }
  }
}

}  // namespace strata

#endif  // STRATA_MEMTABLE_SKIPLIST_H_
