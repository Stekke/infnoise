// Functions used when running in the background

// Required to include kill
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>

#include "infnoise.h"

// Write PID in a file
static bool writePid(int32_t pid, char *fileName) {
    FILE *pidFile = fopen(fileName, "w");
    if(pidFile == NULL) {
        return false;
    }
    int ret = fprintf(pidFile, "%d\n", pid);
    fclose(pidFile);
    return (ret >= 0);
}

// Write end of the pipe that keeps the parent waiting for the device to be ready
static int readyFd = -1;

void startDaemon(struct opt_struct* opts) {
    if(!opts->daemon) {
        // No backgrounding, optionally write current PID
        if(opts->pidFileName != NULL && !writePid(getpid(), opts->pidFileName)) {
            fputs("Unable to write PID file\n", stderr);
            exit(1);
        }
        return;
    }
    int fds[2];
    if(pipe(fds) < 0) {
        fputs("pipe() failed\n", stderr);
        exit(1);
    }
    int32_t pid = fork();
    if(pid < 0) {
        fputs("fork() failed\n", stderr);
        exit(1);
    }
    if(pid > 0) {
        // Parent: only report success once the child is ready.  If it fails, it exits
        // without writing, and read() returns 0.
        char ready;
        ssize_t bytesRead;
        close(fds[1]);
        do {
            bytesRead = read(fds[0], &ready, 1);
        } while(bytesRead < 0 && errno == EINTR);
        if(bytesRead != 1) {
            exit(1);
        }
        if(opts->pidFileName != NULL && !writePid(pid, opts->pidFileName)) {
            // Don't leave a daemon behind that nobody can find
            fputs("Unable to write PID file\n", stderr);
            kill(pid, SIGTERM);
            exit(1);
        }
        exit(0);
    }
    // Child: leave the terminal's session, so that closing the terminal doesn't stop us.
    // stdout and stderr stay open, since they can be the output and the log.
    close(fds[0]);
    setsid();
    if(freopen("/dev/null", "r", stdin) == NULL) {
        fputs("Unable to open /dev/null\n", stderr);
        exit(1);
    }
    readyFd = fds[1];
}

// Let the parent exit, after the device was set up.
void daemonReady(void) {
    if(readyFd >= 0) {
        if(write(readyFd, "1", 1) != 1) {
            fputs("Unable to report that the daemon is ready\n", stderr);
        }
        close(readyFd);
        readyFd = -1;
    }
}
