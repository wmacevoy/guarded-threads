"""
test_guarded.py -- checks for guarded.py.

    python3 -m unittest discover -s python

The Python twin of tests/test_guarded.cpp. Every thread is joined with a
timeout, so a hang shows up as a failure instead of a stuck test run.
"""

import collections
import contextlib
import io
import random
import sys
import threading
import time
import unittest

import factory_guarded
from guarded import Balked, GuardError, Resource, balk, reserve, retry, when

TIMEOUT = 60  # seconds before a stuck thread counts as hung


class Queue(Resource):
    """A bounded queue: data plus its methods, nothing about waiting."""

    def __init__(self, capacity):
        super().__init__()
        self._capacity = capacity
        self._items = collections.deque()

    def empty(self):
        self.const_guard()
        return not self._items

    def full(self):
        self.const_guard()
        return len(self._items) == self._capacity

    def push(self, item):
        self.guard()
        self._items.append(item)

    def pop(self):
        self.guard()
        return self._items.popleft()


def put(q, item):
    with when(lambda: not q.full(), q):
        q.push(item)


def take(q):
    with when(lambda: not q.empty(), q):
        return q.pop()


def transfer(source, target):
    with when(lambda: not source.empty() and not target.full(),
              source, target):
        target.push(source.pop())


def run_threads(test, targets):
    threads = [threading.Thread(target=target) for target in targets]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(TIMEOUT)
        test.assertFalse(thread.is_alive(), "a thread hung")


class TestGuarded(unittest.TestCase):

    def test_pipeline(self):
        """producers -> a -> movers -> b -> consumers, with tiny queues."""
        a, b = Queue(2), Queue(1)
        n = 900  # items: 3 producers x 300, 2 movers x 450, 3 consumers x 300
        got = []

        def produce(p):
            for i in range(300):
                put(a, p * 300 + i)

        def move():
            for _ in range(450):
                transfer(a, b)

        def consume():
            for _ in range(300):
                got.append(take(b))

        run_threads(self,
                    [lambda p=p: produce(p) for p in range(3)]
                    + [move] * 2 + [consume] * 3)
        self.assertEqual(sorted(got), list(range(n)))

    def test_retry_choice(self):
        """retry(): take from whichever queue has something."""
        a, b = Queue(4), Queue(4)
        got = []

        def choose():
            for _ in range(400):
                got.append(retry(
                    lambda: take(a) if not a.empty() else take(b), a, b))

        def fill(q, item):
            for _ in range(200):
                put(q, item)

        run_threads(self, [choose, lambda: fill(a, 1), lambda: fill(b, 2)])
        self.assertEqual(sorted(got), [1] * 200 + [2] * 200)

    def test_floor(self):
        """The factory floor: a machine and three forklifts."""
        floor = factory_guarded.Floor()
        carried = []

        def machine():
            for _ in range(600):
                with reserve(floor):
                    floor.add_box()
            with reserve(floor):
                floor.finish()

        def forklift():
            while factory_guarded.take_box(floor):
                carried.append(1)

        saved = factory_guarded.log
        factory_guarded.log = lambda message: None
        try:
            run_threads(self, [machine] + [forklift] * 3)
        finally:
            factory_guarded.log = saved
        self.assertEqual(len(carried), 600)
        with reserve(floor):
            self.assertEqual(floor.boxes(), 0)

    def test_factory(self):
        """The whole factory, with the jobs sped up."""
        saved = factory_guarded.busy, factory_guarded.log, sys.argv
        factory_guarded.busy = lambda lo, hi: time.sleep(
            random.uniform(lo, hi) / 50)
        factory_guarded.log = lambda message: None
        results = []
        try:
            with contextlib.redirect_stdout(io.StringIO()):  # the summaries
                for args in (["--forklifts", "5", "--trucks", "1",
                              "--bays", "1"],
                             ["--forklifts", "1", "--trucks", "4",
                              "--bays", "2"],
                             []):
                    sys.argv = ["factory_guarded"] + args
                    run_threads(self, [
                        lambda: results.append(factory_guarded.main())])
        finally:
            factory_guarded.busy, factory_guarded.log, sys.argv = saved
        self.assertEqual(results, [0, 0, 0])

    def test_misuse(self):
        a, b = Queue(1), Queue(1)
        with self.assertRaises(GuardError):
            a.empty()                          # not held
        with self.assertRaises(GuardError):
            balk()                             # no guard
        with self.assertRaises(GuardError):
            with reserve(a):
                balk()                         # no retry()
        with self.assertRaises(GuardError):
            with reserve(a):
                with reserve(b):               # nested, new resource
                    pass
        with self.assertRaises(GuardError):
            def change_then_balk():
                a.push(1)
                balk()
            retry(change_then_balk, a)         # balk after a change
        with self.assertRaises(GuardError):
            def changing_condition():
                a.push(2)
                return False
            with when(changing_condition, a):  # condition changes things
                pass
        with self.assertRaises(GuardError):
            when(lambda: True)                 # no resources
        # Nothing is left held after all that.
        self.assertFalse(a.own() or b.own())
        with reserve(a, a, b):                 # duplicates are fine
            self.assertTrue(a.own() and b.own())
        self.assertFalse(a.own() or b.own())

    def test_balk_passes_except_exception(self):
        """A broad except inside a retry() body doesn't swallow a balk."""
        q = Queue(1)
        tries = []

        def body():
            tries.append(1)
            try:
                if q.empty():
                    balk()
            except Exception:
                self.fail("except Exception caught Balked")
            return q.pop()

        def fill():
            time.sleep(0.2)
            put(q, 7)

        got = []
        run_threads(self, [lambda: got.append(retry(body, q)), fill])
        self.assertEqual(got, [7])
        self.assertGreaterEqual(len(tries), 2)
        self.assertFalse(issubclass(Balked, Exception))


if __name__ == "__main__":
    unittest.main()
