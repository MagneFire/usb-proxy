#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utmp.h>

#include <atomic>
#include <pthread.h>

#include "console-shell.h"
#include "misc.h"

namespace {

struct ConsoleShell {
	int master = -1;
	int slave = -1;
	std::atomic<pid_t> pid{0};
	pthread_t keeper = 0;
	bool keeper_running = false;
	std::atomic<bool> stop{false};
	std::string cmd;
};

ConsoleShell g_shell;

pid_t spawn_shell(void)
{
	pid_t pid = fork();
	if (pid < 0) {
		perror("console: fork");
		return -1;
	}
	if (pid > 0)
		return pid;

	// Child. Make the pty slave the controlling terminal and stdio.
	if (login_tty(g_shell.slave) < 0)
		_exit(127);
	// Undo usb-proxy's signal setup: default dispositions, nothing blocked.
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, nullptr);
	for (int s = 1; s < NSIG; s++)
		signal(s, SIG_DFL);
	// Die with usb-proxy (which _exits on device loss without unwinding).
	prctl(PR_SET_PDEATHSIG, SIGHUP);
	// Drop every inherited descriptor: raw-gadget, libusb, the log, the
	// pty master. login_tty already dup2'ed the slave onto 0/1/2.
	int maxfd = (int)sysconf(_SC_OPEN_MAX);
	if (maxfd < 0 || maxfd > 4096)
		maxfd = 4096;
	for (int fd = 3; fd < maxfd; fd++)
		close(fd);
	setenv("TERM", "vt100", 1);
	setenv("HOME", "/", 0);
	execl("/bin/sh", "sh", "-c", g_shell.cmd.c_str(), (char *)nullptr);
	_exit(127);
}

void *keeper_thread(void *)
{
	while (!g_shell.stop) {
		pid_t pid = spawn_shell();
		if (pid < 0) {
			usleep(1000 * 1000);
			continue;
		}
		g_shell.pid = pid;
		printf("[%.3f] console: shell started (pid %d)\n", uptime_s(), pid);
		int status = 0;
		while (waitpid(pid, &status, 0) < 0) {
			if (errno != EINTR)
				break;
		}
		g_shell.pid = 0;
		if (g_shell.stop)
			break;
		printf("[%.3f] console: shell exited (status 0x%x), respawning\n",
		       uptime_s(), status);
		// Backoff so a broken command cannot spin the CPU.
		usleep(500 * 1000);
	}
	return nullptr;
}

} // namespace

bool shell_start(const std::string &cmd)
{
	if (g_shell.master >= 0)
		return true;
	int master, slave;
	if (openpty(&master, &slave, nullptr, nullptr, nullptr) < 0) {
		perror("console: openpty");
		return false;
	}
	fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
	fcntl(master, F_SETFD, FD_CLOEXEC);
	// The parent keeps the slave open so reads on the master never return
	// EIO between two shell instances.
	g_shell.master = master;
	g_shell.slave = slave;
	g_shell.cmd = cmd;
	g_shell.stop = false;
	if (pthread_create(&g_shell.keeper, nullptr, keeper_thread, nullptr) != 0) {
		perror("console: pthread_create");
		close(master);
		close(slave);
		g_shell.master = g_shell.slave = -1;
		return false;
	}
	g_shell.keeper_running = true;
	printf("console: pty %s, command `%s`\n", ptsname(master), cmd.c_str());
	return true;
}

void shell_kill(void)
{
	pid_t pid = g_shell.pid.load();
	if (pid > 0)
		kill(pid, SIGKILL);
}

void shell_stop(void)
{
	if (!g_shell.keeper_running)
		return;
	g_shell.stop = true;
	pid_t pid = g_shell.pid.load();
	if (pid > 0) {
		kill(pid, SIGHUP);
		usleep(50 * 1000);
		if (g_shell.pid.load() == pid)
			kill(pid, SIGKILL);
	}
	pthread_join(g_shell.keeper, nullptr);
	g_shell.keeper_running = false;
	close(g_shell.master);
	close(g_shell.slave);
	g_shell.master = g_shell.slave = -1;
}

int shell_master_fd(void)
{
	return g_shell.master;
}
