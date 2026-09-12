// The shell behind the USB console: a pty whose slave runs a command
// (default `/bin/sh -l`), respawned when it exits. console-acm.cpp moves bytes
// between the pty master and the ACM endpoints.
#ifndef CONSOLE_SHELL_H
#define CONSOLE_SHELL_H

#include <string>

// Create the pty and start the keeper thread that (re)spawns `cmd` on it.
// Returns false if the pty could not be created (console then has no shell;
// the ACM function still enumerates and just echoes nothing).
bool shell_start(const std::string &cmd);
// Kill the shell and stop respawning it. Joins the keeper thread.
void shell_stop(void);
// Async-signal-safe-ish last resort for the _exit paths: SIGKILL the shell
// child, no joins. (The child also carries PR_SET_PDEATHSIG, so this is belt
// and braces.)
void shell_kill(void);
// The pty master (non-blocking), or -1 while no shell exists.
int shell_master_fd(void);

#endif /* CONSOLE_SHELL_H */
