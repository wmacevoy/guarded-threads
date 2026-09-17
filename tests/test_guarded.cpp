//
// test_guarded.cpp -- checks for guarded.hpp.
//
// Queues small enough that threads wait constantly, a wait on two resources
// at once, a retry() whose helper balks, the factory floor, and every misuse
// the library promises to catch. A hang is a failure: the watchdog aborts
// the run after a minute.
//

#undef NDEBUG  // these checks are the test

#include "guarded.hpp"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <thread>
#include <vector>

using namespace guarded;

// A bounded queue: data plus its accessors, nothing about waiting.
class Queue : public Resource {
public:
  explicit Queue(size_t capacity) : capacity(capacity) {}
  bool empty() const { const_guard(); return items.empty(); }
  bool full() const { const_guard(); return items.size() == capacity; }
  size_t size() const { const_guard(); return items.size(); }
  void push(long x) { guard(); items.push_back(x); }
  long pop() { guard(); long x = items.front(); items.pop_front(); return x; }
private:
  size_t capacity;
  std::deque<long> items;
};

void put(Queue &q, long x)
{
  auto hold = when([&] { return !q.full(); }, q);
  q.push(x);
}

long take(Queue &q)
{
  auto hold = when([&] { return !q.empty(); }, q);
  return q.pop();
}

// Waits on two resources at once -- no condition variables, no notify,
// no lock ordering in sight.
void transfer(Queue &from, Queue &to)
{
  auto hold = when([&] { return !from.empty() && !to.full(); }, from, to);
  to.push(from.pop());
}

// The factory floor, in this style.
class Floor : public Resource {
public:
  int boxes() const { const_guard(); return count; }
  bool done() const { const_guard(); return finished; }
  void add() { guard(); ++count; }
  void remove() { guard(); --count; }
  void finish() { guard(); finished = true; }
private:
  int count = 0;
  bool finished = false;
};

bool take_box(Floor &floor)
{
  auto hold = when([&] { return floor.boxes() > 0 || floor.done(); }, floor);
  if (floor.boxes() == 0) return false;
  floor.remove();
  return true;
}

template <typename F>
bool throws_logic(F f)
{
  try { f(); } catch (const std::logic_error &) { return true; }
  return false;
}

int main()
{
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::minutes(1));
    std::cerr << "test_guarded: still running after a minute -- hung?" << std::endl;
    std::abort();
  }).detach();

  // Pipeline: producers -> a -> movers -> b -> consumers. Tiny queues so
  // everyone waits a lot.
  {
    Queue a(2), b(1);
    const long producers = 4, per = 5000, movers = 3, consumers = 5;
    std::atomic<long> sum{0}, count{0};
    std::vector<std::thread> ts;
    for (long p = 0; p < producers; ++p)
      ts.emplace_back([&, p] { for (long i = 0; i < per; ++i) put(a, p * per + i); });
    std::atomic<long> moved{0};
    for (long m = 0; m < movers; ++m)
      ts.emplace_back([&] { while (moved++ < producers * per) transfer(a, b); });
    std::atomic<long> taken{0};
    for (long c = 0; c < consumers; ++c)
      ts.emplace_back([&] { while (taken++ < producers * per) { sum += take(b); ++count; } });
    for (auto &t : ts) t.join();
    long n = producers * per;
    std::cout << "pipeline: " << count << " items, sum " << sum << " (expected " << n * (n - 1) / 2 << ")\n";
    assert(count == n && sum == n * (n - 1) / 2);
  }

  // retry(): take from whichever queue has something. take(b) balks when
  // b is empty too, and the whole block waits for a or b to change.
  {
    Queue a(4), b(4);
    std::atomic<int> got{0};
    std::thread chooser([&] {
      for (int i = 0; i < 1000; ++i) {
        long x = retry([&] { return !a.empty() ? take(a) : take(b); }, a, b);
        assert(x == 1 || x == 2);
        ++got;
      }
    });
    std::thread fa([&] { for (int i = 0; i < 500; ++i) put(a, 1); });
    std::thread fb([&] { for (int i = 0; i < 500; ++i) put(b, 2); });
    fa.join(); fb.join(); chooser.join();
    std::cout << "retry/choice: got " << got << " of 1000\n";
    assert(got == 1000);
  }

  // The factory floor: a machine and three forklifts.
  {
    Floor floor;
    std::atomic<int> carried{0};
    std::vector<std::thread> forklifts;
    for (int f = 0; f < 3; ++f)
      forklifts.emplace_back([&] { while (take_box(floor)) ++carried; });
    std::thread machine([&] {
      for (int i = 0; i < 3000; ++i) { auto hold = reserve(floor); floor.add(); }
      auto hold = reserve(floor);
      floor.finish();
    });
    machine.join();
    for (auto &f : forklifts) f.join();
    auto hold = reserve(floor);
    std::cout << "floor: carried " << carried << " of 3000, " << floor.boxes() << " left\n";
    assert(carried == 3000 && floor.boxes() == 0);
  }

  // Misuse is caught.
  {
    Queue a(1), b(1);
    assert(throws_logic([&] { a.empty(); }));                                // not held
    assert(throws_logic([&] { balk(); }));                                   // no guard
    assert(throws_logic([&] { auto h = reserve(a); balk(); }));              // no retry()
    assert(throws_logic([&] { auto h = reserve(a); auto h2 = reserve(b); })); // nested, new resource
    assert(throws_logic([&] {                                                // write, then balk
      retry([&] { a.push(1); balk(); }, a);
    }));
    assert(throws_logic([&] { auto h = when([&] { a.push(1); return false; }, a); }));  // writing condition
    // Nothing is left held after all that.
    assert(!a.own() && !b.own() && detail::self.held.empty());
    auto h = reserve(a, a, b);  // duplicates are fine
    assert(a.own() && b.own());
    std::cout << "misuse: all caught\n";
  }
  std::cout << "all tests passed\n";
}
