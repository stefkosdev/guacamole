/*
 * Minimal Kin "manager" stub for isolated end-to-end tests.
 *
 * kin_init(NULL, key) runs the Kin library in SERVER mode: it creates the
 * POSIX shared-memory IPC segment /kin_shm_<key>. A service launched with this
 * process's PID as its manager key then attaches as a client and starts
 * normally. This lets us run a SINGLE service (guacamole.service) for testing
 * without the full Kin manager, which would fork ~12 other workers, bind
 * ports, and touch the database.
 *
 * The stub only provides the shm segment and keeps it alive; it does not route
 * IPC messages. That is sufficient because the guacamole Unix-socket path (the
 * Guacamole protocol) is independent of Kin IPC — IPC is only used for the
 * HTTP management API, which these tests do not exercise.
 *
 * Build: linked against kin.library (see scripts/e2e-vnc.sh).
 */
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

extern int kin_init(const char* name, pid_t pid);

static volatile sig_atomic_t running = 1;
static void stop(int sig) { (void)sig; running = 0; }

int main(void)
{
    signal(SIGTERM, stop);
    signal(SIGINT, stop);

    if (kin_init(NULL, (pid_t)getpid()) < 0) {
        fprintf(stderr, "fake_manager: kin_init(server) failed\n");
        return 1;
    }

    /* Announce our PID: the manager key the service must be launched with. */
    printf("%d\n", (int)getpid());
    fflush(stdout);

    while (running) pause();
    return 0;
}
