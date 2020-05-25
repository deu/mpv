/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MP_SUBPROCESS_H_
#define MP_SUBPROCESS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mp_cancel;

typedef void (*subprocess_read_cb)(void *ctx, char *data, size_t size);

void mp_devnull(void *ctx, char *data, size_t size);

#define MP_SUBPROCESS_MAX_FDS 10

struct mp_subprocess_fd {
    int fd;                         // target FD

    // Only one of on_read or src_fd can be set. If none are set, use /dev/null.
    // Note: "neutral" initialization requires setting src_fd=-1.
    subprocess_read_cb on_read;     // if not NULL, serve reads
    void *on_read_ctx;              // for on_read(on_read_ctx, ...)
    int src_fd;                     // if >=0, dup this FD to target FD
};

struct mp_subprocess_opts {
    char *exe;      // binary to execute (never non-NULL)
    char **args;    // argument list (NULL for none, otherwise NULL-terminated)
    char **env;     // if !NULL, set this as environment variable block
    // Complete set of FDs passed down. All others are supposed to be closed.
    struct mp_subprocess_fd fds[MP_SUBPROCESS_MAX_FDS];
    int num_fds;
    struct mp_cancel *cancel; // if !NULL, asynchronous process abort (kills it)
    bool detach;    // if true, do not wait for process to end
};

struct mp_subprocess_result {
    int error;              // one of MP_SUBPROCESS_* (>0 on error)
    // NB: if WIFEXITED applies, error==0, and this is WEXITSTATUS
    //     on win32, this can use the full 32 bit
    //     if started with detach==true, this is always 0
    uint32_t exit_status;   // if error==0==MP_SUBPROCESS_OK, 0 otherwise
};

// Subprocess error values.
#define MP_SUBPROCESS_OK                0   // no error
#define MP_SUBPROCESS_EGENERIC          -1  // unknown error
#define MP_SUBPROCESS_EKILLED_BY_US     -2  // mp_cancel was triggered
#define MP_SUBPROCESS_EINIT             -3  // error during initialization
#define MP_SUBPROCESS_EUNSUPPORTED      -4  // API not supported

// Turn MP_SUBPROCESS_* values into a static string. Never returns NULL.
const char *mp_subprocess_err_str(int num);

// Caller must set *opts.
void mp_subprocess2(struct mp_subprocess_opts *opts,
                    struct mp_subprocess_result *res);

// Start a subprocess. Uses callbacks to read from stdout and stderr.
// Returns any of MP_SUBPROCESS_*, or a value >=0 for the process exir
int mp_subprocess(char **args, struct mp_cancel *cancel, void *ctx,
                  subprocess_read_cb on_stdout, subprocess_read_cb on_stderr,
                  char **error);

struct mp_log;
void mp_subprocess_detached(struct mp_log *log, char **args);

#endif
