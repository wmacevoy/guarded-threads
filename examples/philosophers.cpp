//
// philosophers.cpp -- the dining philosophers on guarded.hpp.
//
// Philosophers sit around a table with one fork between each pair, and a
// philosopher needs both neighboring forks to eat. The classic way to get
// this wrong: everyone picks up their left fork, then waits forever for the
// right one.
//
// Here a philosopher asks for both forks at once:
//
//   auto hold = reserve(left, right);
//
// and reserve() always takes resources in the same (address) order, so that
// circle of waiting threads can't form. Nobody has to number the forks or
// think about lock order.
//

#include "guarded.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using guarded::Resource;
using guarded::reserve;

std::mutex print_lock;

void say(int philosopher, const std::string &what)
{
  std::scoped_lock hold(print_lock);
  std::cout << "philosopher " << philosopher << " " << what << std::endl;
}

void nap(int max_ms)
{
  thread_local std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<int> ms(0, max_ms);
  std::this_thread::sleep_for(std::chrono::milliseconds(ms(rng)));
}

class Fork : public Resource {
public:
  void use() { guard(); ++uses; }
  int used() const { const_guard(); return uses; }

private:
  int uses = 0;
};

void philosopher(int me, Fork &left, Fork &right, int meals)
{
  for (int meal = 1; meal <= meals; ++meal) {
    say(me, "is thinking");
    nap(20);
    auto hold = reserve(left, right);
    left.use();
    right.use();
    say(me, "is eating meal " + std::to_string(meal));
    nap(20);
  }
  say(me, "is full");
}

}  // namespace

int main(int argc, char *argv[])
{
  int philosophers = 5;
  int meals = 3;
  bool args_ok = argc % 2 == 1;  // --<option> <#> pairs
  for (int argi = 1; args_ok && argi + 1 < argc; argi += 2) {
    if (std::strcmp(argv[argi], "--philosophers") == 0) {
      philosophers = std::atoi(argv[argi + 1]);
    } else if (std::strcmp(argv[argi], "--meals") == 0) {
      meals = std::atoi(argv[argi + 1]);
    } else {
      args_ok = false;
    }
  }
  if (!args_ok || philosophers < 1 || philosophers > 1000 || meals < 1) {
    std::cerr << "usage: " << argv[0]
              << " [--philosophers <#>] [--meals <#>]" << std::endl;
    return 1;
  }

  // Fork can't be moved (it holds a mutex), so the vector holds pointers.
  std::vector<std::unique_ptr<Fork>> forks;
  for (int i = 0; i < philosophers; ++i) {
    forks.push_back(std::make_unique<Fork>());
  }

  std::vector<std::thread> table;
  for (int i = 0; i < philosophers; ++i) {
    Fork &left = *forks[i];
    Fork &right = *forks[(i + 1) % philosophers];
    table.emplace_back(philosopher, i, std::ref(left), std::ref(right), meals);
  }
  for (std::thread &seat : table) {
    seat.join();
  }

  // Each fork is used once per meal by each of its two neighbors.
  bool ok = true;
  for (auto &fork : forks) {
    auto hold = reserve(*fork);
    ok = ok && fork->used() == 2 * meals;
  }
  std::cout << (ok ? "everyone ate" : "fork count is wrong") << std::endl;
  return ok ? 0 : 1;
}
