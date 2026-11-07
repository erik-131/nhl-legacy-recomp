// Stress test for lost and stolen event wakeups in rex::thread (POSIX). Prints counts per scenario; 0 is correct.
// Build: see tools/nuc/tests/build_event_wakeups.sh. Run with LD_LIBRARY_PATH pointing at the librexruntime.so to test.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include <rex/thread.h>

using namespace std::chrono_literals;
using rex::thread::Event;
using rex::thread::WaitResult;

// A manual-reset event set and reset right away must still release a thread already waiting on it.
static int ManualPulseMisses(int rounds) {
  auto ev = Event::CreateManualResetEvent(false);
  std::atomic<int> waiting{0};
  int misses = 0;
  std::thread waiter([&] {
    for (int i = 0; i < rounds; ++i) {
      waiting.store(i + 1);
      if (rex::thread::Wait(ev.get(), false, 50ms) != WaitResult::kSuccess) ++misses;
    }
  });
  for (int i = 0; i < rounds; ++i) {
    while (waiting.load() != i + 1) std::this_thread::yield();
    std::this_thread::sleep_for(100us);  // Lets the waiter block on the event.
    ev->Set();
    ev->Reset();
    std::this_thread::sleep_for(100us);
  }
  waiter.join();
  return misses;
}

// SignalAndWait is atomic: the thread woken by the first signal can't take the second signal away from it.
static int SignalAndWaitStolen(int rounds) {
  auto a = Event::CreateAutoResetEvent(false), b = Event::CreateAutoResetEvent(false);
  int stolen = 0;
  std::thread partner([&] {
    for (int i = 0; i < rounds; ++i) {
      if (rex::thread::Wait(a.get(), false, 500ms) != WaitResult::kSuccess) continue;
      b->Set();
      if (rex::thread::Wait(b.get(), false, 0ms) == WaitResult::kSuccess) {
        ++stolen;
        b->Set();  // Give it back so the other side carries on.
      }
    }
  });
  for (int i = 0; i < rounds; ++i) rex::thread::SignalAndWait(a.get(), b.get(), false, 500ms);
  partner.join();
  return stolen;
}

int main() {
  const int misses = ManualPulseMisses(2000);
  const int stolen = SignalAndWaitStolen(200000);
  std::printf("manual set+reset missed %d/2000, SignalAndWait signal stolen %d/200000\n", misses, stolen);
  return misses || stolen;
}
