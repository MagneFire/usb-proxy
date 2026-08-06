#ifndef POWER_POLICY_H
#define POWER_POLICY_H

#include <cstdint>
#include <string>

// Dynamic power policy (--power_hook / --power_idle_ms, or config.json
// "power_hook" / "power_idle_ms").
//
// usb-proxy knows when USB traffic is flowing; it does not, and should not,
// know how a particular board saves power. So it just calls an external hook
// with "active" or "idle" on each transition and leaves the board-specific
// knobs (CPU hotplug, cpufreq, clock gates) to that script. On the Orange Pi
// appliance the hook is /usr/bin/power-tune, which lives on a RAM rootfs and
// can therefore be retuned over the serial console without rebuilding this
// binary.
//
// The wind-up is triggered from the forwarding threads and is immediate; the
// wind-down happens after power_idle_ms of no traffic, from a monitor thread
// that wakes once a second. Nothing forks on the data path.

// Path to the hook program. Empty (the default) disables the whole feature,
// including the monitor thread.
extern std::string power_hook;

// Quiet period before winding down, in milliseconds.
extern int power_idle_ms;

// Floor for power_idle_ms, and the reason there is one. The hook is free to do
// something heavyweight per transition — on the Orange Pi appliance it hotplugs
// a CPU, which is a stop_machine. Measured there 2026-08-06: 50 back-to-back
// offline/online cycles hard-reset the board (watchdog, no oops in pstore),
// while 10 cycles spaced 2s apart were clean. Winding up and down cannot happen
// faster than once per power_idle_ms, so a floor of 1s keeps the transition rate
// an order of magnitude inside the safe zone no matter how it is configured.
#define POWER_IDLE_MS_MIN 1000

// Called from the forwarding threads for every transfer that moved data.
// Cheap by design: one relaxed atomic add, plus one atomic load that only
// does more work on an idle -> active transition.
void power_note_activity(uint64_t bytes);

// Spawn the monitor thread. No-op when power_hook is empty. Call once, after
// options are parsed.
void power_policy_start(void);

#endif /* POWER_POLICY_H */
