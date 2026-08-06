#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>

#include "power-policy.h"

extern char **environ;
extern int verbose_level;

std::string power_hook;
int power_idle_ms = 5000;

namespace {

std::atomic<uint64_t> activity_bytes(0);

// True once the hot path has signalled a wind-up and until the monitor has
// wound back down. Doubles as the "already signalled" guard, so a busy
// transfer signals the monitor exactly once, not once per packet.
std::atomic<bool> wound_up_signalled(false);

std::mutex mtx;
std::condition_variable cv;
bool wake_now = false;

void run_hook(const char *arg)
{
	char *const argv[] = {
		const_cast<char *>(power_hook.c_str()),
		const_cast<char *>(arg),
		NULL,
	};
	pid_t pid;

	int rc = posix_spawn(&pid, power_hook.c_str(), NULL, NULL, argv, environ);
	if (rc != 0) {
		fprintf(stderr, "[power] cannot run %s %s: %s\n",
			power_hook.c_str(), arg, strerror(rc));
		return;
	}
	waitpid(pid, NULL, 0);
	if (verbose_level)
		printf("[power] %s\n", arg);
}

void *power_monitor(void *)
{
	using clock = std::chrono::steady_clock;

	// A device has just enumerated, so traffic is imminent: start wound up
	// and let the idle timer take us down. Winding down here instead would
	// fight the enumeration burst.
	run_hook("active");
	wound_up_signalled.store(true, std::memory_order_relaxed);
	bool wound_up = true;

	uint64_t last_bytes = activity_bytes.load(std::memory_order_relaxed);
	auto last_change = clock::now();

	for (;;) {
		bool woke;
		{
			std::unique_lock<std::mutex> lock(mtx);
			cv.wait_for(lock, std::chrono::seconds(1),
				    [] { return wake_now; });
			woke = wake_now;
			wake_now = false;
		}

		if (woke && !wound_up) {
			run_hook("active");
			wound_up = true;
		}

		uint64_t now_bytes = activity_bytes.load(std::memory_order_relaxed);
		auto now = clock::now();

		if (now_bytes != last_bytes) {
			last_bytes = now_bytes;
			last_change = now;
			continue;
		}

		if (!wound_up)
			continue;

		auto quiet = std::chrono::duration_cast<std::chrono::milliseconds>(
				now - last_change).count();
		if (quiet < power_idle_ms)
			continue;

		// Re-arm the hot-path trigger *before* running the hook: traffic
		// arriving while the hook runs then sets wake_now, and the next
		// iteration winds straight back up. The other order would drop
		// that wakeup and leave us wound down with traffic flowing.
		wound_up_signalled.store(false, std::memory_order_relaxed);
		run_hook("idle");
		wound_up = false;
	}
	return NULL;
}

} // namespace

void power_note_activity(uint64_t bytes)
{
	if (power_hook.empty())
		return;

	activity_bytes.fetch_add(bytes, std::memory_order_relaxed);

	// Common case (already wound up): one relaxed load and a predictable
	// not-taken branch. Only the first transfer after an idle period wins
	// the exchange and pays for the lock + notify.
	if (wound_up_signalled.exchange(true, std::memory_order_relaxed))
		return;

	{
		std::lock_guard<std::mutex> lock(mtx);
		wake_now = true;
	}
	cv.notify_one();
}

void power_policy_start(void)
{
	if (power_hook.empty())
		return;

	pthread_t thread;
	if (pthread_create(&thread, NULL, power_monitor, NULL)) {
		fprintf(stderr, "[power] cannot start monitor thread; "
				"dynamic power policy disabled\n");
		power_hook.clear();
		return;
	}
	pthread_detach(thread);
	printf("power policy enabled (hook %s, idle after %dms)\n",
	       power_hook.c_str(), power_idle_ms);
}
