#include "ready_queue.hpp"

#include <utility>

namespace tcu::scheduler::detail {

bool ReadyQueue::push(ReadyJob&& job) {
  if (full()) return false;
  jobs_.push_back(std::move(job));
  return true;
}

ReadyJob ReadyQueue::pop() {
  ReadyJob job = std::move(jobs_.front());
  jobs_.pop_front();
  return job;
}

}  // namespace tcu::scheduler::detail
