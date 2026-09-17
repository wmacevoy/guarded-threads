# guarded-threads

[![CI](https://github.com/wmacevoy/guarded-threads/actions/workflows/ci.yml/badge.svg)](https://github.com/wmacevoy/guarded-threads/actions/workflows/ci.yml)

Wait by condition, not by signal. A small library, in C++17 and Python,
where a thread says *what it needs* and *what it is waiting for*, and never
touches a condition variable:

```cpp
void transfer(Queue &from, Queue &to)
{
  auto hold = when([&] { return !from.empty() && !to.full(); }, from, to);
  to.push(from.pop());
}
```

```python
def transfer(source, target):
    with when(lambda: not source.empty() and not target.full(), source, target):
        target.push(source.pop())
```

`when()` locks both queues, in an order every thread agrees on, and checks
the condition. If the condition is false, it lets go of everything and sleeps
until some other thread changes one of the queues, then checks again. When
the condition holds, the block runs with both queues locked.

What that removes, compared with condition variables:

- the `while (!ready) wait()` loop, and the bug of writing `if` instead;
- choosing which condition variable to wait on, and which to signal;
- the forgotten `notify`, and the subtle `notify_one` that wakes the wrong
  thread;
- lock ordering, and the deadlocks that come from getting it wrong;
- unlocked access: every accessor checks that the caller holds the resource,
  so a forgotten lock throws instead of racing.

It is meant for programs with a modest number of threads that coordinate --
simulations, pipelines, background workers, test harnesses, and teaching.
Read [Caveats](#caveats) before using it anywhere busy.

## Contents

| Path | What it is |
|---|---|
| `include/guarded.hpp` | The C++17 library (header only) |
| `python/guarded.py` | The Python library (one module, standard library only) |
| `examples/factory_condvar.cpp` | A factory simulation with ordinary condition variables |
| `examples/factory_guarded.cpp` | The same factory on `guarded.hpp` -- compare the two |
| `examples/philosophers.cpp` | Dining philosophers, deadlock-free with one `reserve(left, right)` |
| `examples/queues.cpp` | Waiting on one queue, on two at once, and on whichever is ready |
| `python/factory_condvar.py` | The factory in Python with `threading.Condition` |
| `python/factory_guarded.py` | The same factory on `guarded.py` |
| `tests/test_guarded.cpp` | C++ tests |
| `python/test_guarded.py` | Python tests |

## Writing a resource

A resource is a class derived from `Resource`. Its data is private, and every
accessor starts with a guard call. A const accessor calls `const_guard()`; any
other calls `guard()`. Both throw if the calling thread does not hold the
resource. `guard()` also marks the resource as changed, and releasing a
changed resource wakes the threads waiting on it.

```cpp
#include "guarded.hpp"

class Queue : public guarded::Resource {
public:
  explicit Queue(size_t capacity) : capacity(capacity) {}

  bool empty() const { const_guard(); return items.empty(); }
  bool full() const { const_guard(); return items.size() == capacity; }

  void push(long item) { guard(); items.push_back(item); }
  long pop() { guard(); long item = items.front(); items.pop_front(); return item; }

private:
  size_t capacity;
  std::deque<long> items;
};
```

```python
import collections

from guarded import Resource

class Queue(Resource):
    def __init__(self, capacity):
        super().__init__()                  # required
        self._capacity = capacity
        self._items = collections.deque()

    def empty(self):
        self.const_guard()
        return not self._items

    def push(self, item):
        self.guard()
        self._items.append(item)
```

Python has no `const`, so choosing between `const_guard()` and `guard()` is up
to you. In C++ the compiler rejects `guard()` inside a const accessor.

## Using resources

| C++ | Python | Meaning |
|---|---|---|
| `auto hold = when(ready, a, b);` | `with when(ready, a, b):` | Wait until `ready()` is true, then hold `a` and `b` for the scope |
| `auto hold = reserve(a, b);` | `with reserve(a, b):` | Hold `a` and `b` for the scope (no condition) |
| `retry(body, a, b)` | `retry(body, a, b)` | Run `body` holding `a` and `b`; if it balks, wait for a change and run it again. Returns what `body` returns |
| `balk()` | `balk()` | Give up on this attempt (inside a `retry`) |
| `r.own()` | `r.own()` | Does the calling thread hold `r`? |

Most code needs only `when()`, with the condition up front:

```cpp
long take(Queue &q)
{
  auto hold = when([&] { return !q.empty(); }, q);
  return q.pop();
}
```

`retry()` is for code that finds out partway through that it has to wait. A
`when()` nested inside another guard doesn't wait: it checks that the outer
guard already holds its resources, and if its condition is false, it balks.
So a helper like `take()` works on its own *and* inside a `retry()`:

```cpp
// Take from whichever queue has something. When both are empty, take(b)
// balks, and retry() waits until a or b changes.
long take_either(Queue &a, Queue &b)
{
  return retry([&] { return !a.empty() ? take(a) : take(b); }, a, b);
}
```

## Rules the library enforces

Breaking one of these throws `std::logic_error` in C++ and `GuardError` in
Python:

- Using a resource you don't hold.
- Nesting a guard that names a resource the outermost guard doesn't hold.
  Taking something new while holding other things is how deadlocks start, so
  the outermost guard must name everything.
- Calling `balk()` outside a `retry()` (including inside a plain `when()` or
  `reserve()` block).
- Calling `balk()` after changing a resource. Nothing is undone, so running
  the block again would repeat the change.
- A `when()` condition that changes a resource.
- Naming no resources at all.

## How it works

1. **Take everything at once.** The resources a guard names are sorted by
   address (`id()` in Python), duplicates removed, and locked in that order.
   Every thread uses the same order, so no cycle of waiting threads can form.
2. **Check the condition** with everything held.
3. **If it is false, park.** While still holding everything, the thread adds
   itself to each resource's list of waiters. Then it releases everything and
   sleeps on its own condition variable. Signing up before letting go means no
   change can slip by unseen.
4. **Release wakes waiters.** Releasing a resource that `guard()` marked as
   changed takes its waiter list, unlocks, and wakes everyone on the list.
   Releasing an unchanged resource wakes no one.
5. **A woken thread starts over** at step 1, taking itself off the lists as it
   locks each resource.
6. **Balking** throws `Balked`, which the outermost `retry()` catches: it parks
   (step 3) and then runs the block again.

Waiting on *any* of several resources would, with plain pthreads, mean waiting
on several condition variables at once, and there is no such call. Step 3 turns
that around: one condition variable per thread, and a list of waiting threads
per resource. Go's `select` waits on several channels the same way.

## Caveats

- **Every change wakes every thread waiting on that resource.** Each one
  wakes, locks everything it named, rechecks its condition, and probably goes
  back to sleep. With a few waiters per resource this costs little. With
  hundreds waiting on one resource, each change sets off hundreds of wakeups.
  (Abseil's `Mutex` avoids this by having the unlocking thread evaluate the
  waiters' conditions itself; that needs conditions that are safe to run on
  another thread.)
- **Resources are coarse, and named up front.** A thread can't lock just the
  part it turns out to need: a truck that wants one loading bay locks the
  whole dock. Taking everything at once is what rules out deadlock, and this
  is its price. Split big resources if contention hurts.
- **Nothing is undone.** Balk before you change anything -- including side
  effects the library can't see, like printing. A retried block must be safe
  to run again up to the point where it balks.
- **Conditions run with the resources held.** Keep them quick, don't block in
  them, and don't take other locks in them.
- **Checks happen at run time**, and only on code paths that run. In C++, a
  non-const accessor that calls `const_guard()` by mistake compiles, and never
  wakes anyone, so threads waiting on that resource can sleep forever. Clang's
  `-Wthread-safety` annotations could move some checks to compile time; this
  library doesn't use them.
- **No fairness, timeouts, or priorities.** A thread can keep losing to
  others, and a condition that never becomes true waits forever.
- **Holding a resource while you block is allowed** -- the factory's forklifts
  hold the dock door while they drive -- but everyone who needs that resource
  waits too.
- **Other locks must be leaves.** A plain mutex used inside a guard (the
  examples' `print_lock`) is safe only if nothing else is ever locked while it
  is held. A guarded resource can't be taken while holding it, by the nesting
  rule.
- **Balking is an exception.** That's fine at human-scale rates, but slow in a
  hot loop; a top-level `when()` never throws. In C++ a `catch (...)` that
  doesn't rethrow swallows a balk (`Balked` is not a `std::exception`). In
  Python `Balked` derives from `BaseException`, so `except Exception:` lets it
  through, but a bare `except:` doesn't.
- **Thread ids can be reused.** `own()` compares thread ids, which the system
  may hand to a new thread after one exits. That only matters if a thread
  exits while holding a resource, and the scoped holds prevent that.
- **Lifetimes.** Every resource must outlive the guards that name it. A C++
  `Resource` holds a mutex, so it can be neither copied nor moved; keep them
  in `std::unique_ptr` if they live in a container. A Python subclass that
  defines `__init__` must call `super().__init__()`.
- **Python threads don't run Python code in parallel** (the GIL). The library
  is about coordination, not speed. It has been tested on CPython 3.12 and
  3.13 with the GIL, not yet on free-threaded builds.

## Building and testing

Needs a C++17 compiler, GNU make, and Python 3.8 or later.

```
make                # examples and the C++ tests, into bin/
make test           # C++ tests, every example, and the Python tests
make sanitize       # C++ tests under ThreadSanitizer, then ASan and UBSan
bin/factory_guarded --forklifts 3 --trucks 3 --bays 2 --trips 2 --capacity 3
python3 python/factory_guarded.py --forklifts 3 --trucks 3
```

The C++ library is the single file `include/guarded.hpp`; the Python library is
the single file `python/guarded.py`. Copy either one into your project.
`compile_flags.txt` tells clangd how to build the examples.

ThreadSanitizer can fail to start inside Docker, which blocks it from turning
off address randomization. That is a sandbox limit, not a library problem.

## Prior art

- **Conditional critical regions** -- `region v when B do S` -- from C. A. R.
  Hoare, "Towards a Theory of Parallel Programming" (1972), and Per Brinch
  Hansen, "Structured Multiprogramming" (1972). Monitors with explicit
  condition variables replaced them, largely because re-checking every
  condition on every release was considered too expensive.
- **Ada protected objects** re-check each entry's barrier automatically when a
  protected action finishes.
- **Automatic-signal monitors**, in P. A. Buhr, M. Fortier, and M. H. Coffin,
  "Monitor Classification" (ACM Computing Surveys, 1995).
- **`retry` in software transactional memory**: T. Harris, S. Marlow,
  S. Peyton Jones, and M. Herlihy, "Composable Memory Transactions" (2005).
  `balk()` is its lock-based cousin, without the rollback.
- **Abseil's `absl::Mutex`**, whose `LockWhen(Condition)` and
  `Await(Condition)` are an alternative to its condition variable.

## License

MIT -- see [LICENSE](LICENSE). Each library file carries the notice, so a
copied `guarded.hpp` or `guarded.py` keeps it.
