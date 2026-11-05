#include "deadline_queue.hpp"

#include <algorithm>

namespace tcu::scheduler::detail {

bool DeadlineQueue::later(const HeapEntry& a, const HeapEntry& b) {
  if (a.deadline != b.deadline) return a.deadline > b.deadline;
  return a.id > b.id;
}

bool DeadlineQueue::isStale(const HeapEntry& entry, const Registry& registry) {
  const auto it = registry.find(entry.id);
  if (it == registry.end()) return true;
  return it->second->generation != entry.generation;
}

void DeadlineQueue::push(const HeapEntry& entry) {
  heap_.push_back(entry);
  std::push_heap(heap_.begin(), heap_.end(), &DeadlineQueue::later);
}

void DeadlineQueue::pop() {
  std::pop_heap(heap_.begin(), heap_.end(), &DeadlineQueue::later);
  heap_.pop_back();
}

void DeadlineQueue::pruneAndCompact(const Registry& registry) {
  while (!heap_.empty() && isStale(heap_.front(), registry)) {
    pop();
    if (staleEstimate_ > 0) --staleEstimate_;
  }
  if (heap_.size() > kCompactionFloor && staleEstimate_ * 2 > heap_.size()) {
    compact(registry);
  }
}

void DeadlineQueue::compact(const Registry& registry) {
  heap_.erase(std::remove_if(heap_.begin(), heap_.end(),
                             [&registry](const HeapEntry& e) {
                               return isStale(e, registry);
                             }),
              heap_.end());
  std::make_heap(heap_.begin(), heap_.end(), &DeadlineQueue::later);
  staleEstimate_ = 0;
}

}  // namespace tcu::scheduler::detail
