//
// queues.cpp -- bounded queues on guarded.hpp.
//
// Three ways to wait:
//
//   put / take      wait on one queue          when(cond, q)
//   transfer        wait on two queues at once when(cond, from, to)
//   take_either     wait for whichever queue   retry(body, a, b) + balk
//
// Producers fill queue a and queue b. A mover transfers items from a to c.
// A consumer takes from whichever of b and c has something. Every queue is
// tiny, so everyone waits a lot.
//

#include "guarded.hpp"

#include <deque>
#include <iostream>
#include <thread>
#include <vector>

namespace {

using guarded::Resource;
using guarded::retry;
using guarded::when;

// Data and accessors -- nothing about waiting.
class Queue : public Resource {
public:
  explicit Queue(size_t capacity) : capacity(capacity) {}

  bool empty() const { const_guard(); return items.empty(); }
  bool full() const { const_guard(); return items.size() == capacity; }

  void push(long item) { guard(); items.push_back(item); }

  long pop()
  {
    guard();
    long item = items.front();
    items.pop_front();
    return item;
  }

private:
  size_t capacity;
  std::deque<long> items;
};

void put(Queue &q, long item)
{
  auto hold = when([&] { return !q.full(); }, q);
  q.push(item);
}

long take(Queue &q)
{
  auto hold = when([&] { return !q.empty(); }, q);
  return q.pop();
}

// Plain monitors make this hard: it needs both locks, in a safe order, and
// a wait that wakes for a change to either queue.
void transfer(Queue &from, Queue &to)
{
  auto hold = when([&] { return !from.empty() && !to.full(); }, from, to);
  to.push(from.pop());
}

// When both are empty, take(b) balks, and retry() waits until a or b
// changes, then runs the lambda again.
long take_either(Queue &a, Queue &b)
{
  return retry([&] { return !a.empty() ? take(a) : take(b); }, a, b);
}

}  // namespace

int main()
{
  const long items = 1000;  // from each producer
  Queue a(2), b(2), c(1);

  std::thread produce_a([&] { for (long i = 0; i < items; ++i) put(a, 1); });
  std::thread produce_b([&] { for (long i = 0; i < items; ++i) put(b, 2); });
  std::thread mover([&] { for (long i = 0; i < items; ++i) transfer(a, c); });

  long from_a = 0, from_b = 0;
  std::thread consumer([&] {
    for (long i = 0; i < 2 * items; ++i) {
      (take_either(b, c) == 1 ? from_a : from_b) += 1;
    }
  });

  produce_a.join();
  produce_b.join();
  mover.join();
  consumer.join();

  std::cout << "consumed " << from_a << " items that went a -> c, and "
            << from_b << " that came straight from b" << std::endl;
  return from_a == items && from_b == items ? 0 : 1;
}
