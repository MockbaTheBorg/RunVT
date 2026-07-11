// abstraction_posix.h - RunVT OS abstraction layer, POSIX/Linux side.
//
// Every bit of process-spawning and pty I/O for Linux lives in this
// one file. Nothing outside abstraction_posix.h / abstraction_windows.h
// should need to know whether it's running on Linux or Windows -
// that's the whole point of splitting it out like this.

#ifndef RUNVT_ABSTRACTION_POSIX_H
#define RUNVT_ABSTRACTION_POSIX_H

#include <pty.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <termios.h>

typedef struct RT_Process {
    int master_fd;
    pid_t pid;
    int exited;
    int exit_code;
} RT_Process;

// Spawns argv[0] with args argv (NULL-terminated) as a child attached
// to a fresh pty. cols/rows become the pty's initial window size, so
// the child sees the right dimensions from the start instead of
// guessing 80x24 and being wrong half the time. Returns 0 on success,
// -1 on failure.
static int rt_spawn(RT_Process *proc, char *const argv[], int cols, int rows) {
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;

    int master;
    pid_t pid = forkpty(&master, NULL, NULL, &ws);
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        // child: the pty slave is already our controlling tty and our
        // stdin/stdout/stderr, courtesy of forkpty()
        setenv("TERM", "vt100", 1);
        execvp(argv[0], argv);
        _exit(127); // only reached if execvp itself failed
    }

    // parent
    proc->master_fd = master;
    proc->pid = pid;
    proc->exited = 0;
    proc->exit_code = 0;

    // Turn off software flow control. Without this, ^S/^Q on the pty's
    // line discipline get intercepted by the kernel as XOFF/XON instead
    // of reaching the app as plain bytes - which is exactly what a
    // fresh pty defaults to, and exactly what bit WordStar-style
    // editors (^S/^D for cursor left/right) running under us: hit ^S to
    // move left and the whole pty output just freezes until an XON
    // nothing sends ever arrives. A real interactive shell usually
    // papers over this with its own `stty -ixon`, but we're not running
    // a shell first, so it's on us to set it here.
    struct termios tio;
    if (tcgetattr(master, &tio) == 0) {
        tio.c_iflag &= ~((tcflag_t)(IXON | IXOFF));
        tcsetattr(master, TCSANOW, &tio);
    }

    // we poll this fd every frame, so nonblocking is a must
    fcntl(master, F_SETFL, fcntl(master, F_GETFL, 0) | O_NONBLOCK);

    return 0;
}

// Blocks up to timeout_ms waiting for data from the child. Returns 1
// if there's something to read, 0 on timeout, -1 on error. A negative
// timeout means block forever (not used anywhere right now, but no
// reason to rule it out).
static int rt_poll_readable(RT_Process *proc, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(proc->master_fd, &rfds);

    struct timeval tv;
    struct timeval *tvp;
    if (timeout_ms < 0) {
        tvp = NULL;
    } else {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }

    int r = select(proc->master_fd + 1, &rfds, NULL, NULL, tvp);
    if (r < 0) {
        if (errno == EINTR) {
            return 0;
        }
        return -1;
    }
    return r > 0 ? 1 : 0;
}

// Nonblocking read from the child. Returns bytes read (>0), 0 if there
// was nothing to read right now, -1 if the pty's gone (child exited
// and closed its end).
static int rt_read(RT_Process *proc, unsigned char *buf, int maxlen) {
    int n = (int)read(proc->master_fd, buf, (size_t)maxlen);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return 0;
        }
        return -1;
    }
    if (n == 0) {
        return -1;
    }
    return n;
}

// Writes bytes to the child's stdin (keyboard input, DSR/DA replies).
static int rt_write(RT_Process *proc, const unsigned char *buf, int len) {
    return (int)write(proc->master_fd, buf, (size_t)len);
}

// Nonblocking check for whether the child has exited. Returns 1 if
// it's still running, 0 once it's gone (exit code lands in
// proc->exit_code, shown in the --wait prompt).
static int rt_child_alive(RT_Process *proc) {
    if (proc->exited) {
        return 0;
    }

    int status;
    pid_t r = waitpid(proc->pid, &status, WNOHANG);
    if (r == proc->pid) {
        proc->exited = 1;
        proc->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return 0;
    }
    return 1;
}

static void rt_cleanup(RT_Process *proc) {
    if (proc->master_fd >= 0) {
        close(proc->master_fd);
        proc->master_fd = -1;
    }
    if (!proc->exited && proc->pid > 0) {
        kill(proc->pid, SIGHUP);
        waitpid(proc->pid, NULL, 0);
    }
}

#endif // RUNVT_ABSTRACTION_POSIX_H
