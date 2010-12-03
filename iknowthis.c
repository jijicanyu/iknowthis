#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#include <asm/unistd.h>
#include <errno.h>
#include <glib.h>
#include <sched.h>
#include <search.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <microhttpd.h>
#include <ClearSilver.h>
#include <pwd.h>
#include <grp.h>

#include "sysfuzz.h"
#include "typelib.h"
#include "iknowthis.h"

syscall_fuzzer_t  *system_call_fuzzers;                  // Fuzzer definitions for each system call.
guint              total_registered_fuzzers;             // Total number of registered fuzzers.
guint              total_disabled_fuzzers;               // Number of fuzzers that have been disabled.
guint              process_nesting_depth;                // Nested process depth.
guint              skip_danger_warning;                  // Dont print the warning message on startup.
gint               semid;                                // Semaphore set for syscall fuzzers.

static void create_watchdog_process(void);
static gint httpd_connect_policy(gpointer cls, const struct sockaddr *addr, socklen_t addrlen);
static gint httpd_access_handler(gpointer cls,
                                 struct MHD_Connection *connection,
                                 const gchar *url,
                                 const gchar *method,
                                 const gchar *version,
                                 const gchar *upload_data,
                                 gsize *upload_data_size,
                                 gpointer *con_cls);

static struct sembuf takelock[] = {
    {
        .sem_num    = 0,
        .sem_op     = 0,    // Wait for zero
        .sem_flg    = 0,
    },
    {
        .sem_num    = 0,
        .sem_op     = 1,    // Increment
        .sem_flg    = SEM_UNDO,
    },
};

static struct sembuf releaselock[] = {
    {
        .sem_num    = 0,
        .sem_op     = -1,   // Decrement
        .sem_flg    = SEM_UNDO,
    },
};


void create_fuzzer_report(HDF *hdf);
static void print_danger_warning(void);
static gboolean disable_enable_fuzzer_range(const gchar *option_name, const gchar *value, gpointer data, GError **error);
static gboolean list_fuzzer_names(const gchar *option_name, const gchar *value, gpointer data, GError **error);

// Command line options.
static GOptionEntry parameters[] = {
    { "dangerous",         0, 0,                    G_OPTION_ARG_NONE,     &skip_danger_warning,        "Do not display warning about system damage", NULL },
    { "disable",           0, 0,                    G_OPTION_ARG_CALLBACK, disable_enable_fuzzer_range, "Disable fuzzers specified in range", "1,2,mincore,43-63,mq_*,..." },
    { "enable",            0, 0,                    G_OPTION_ARG_CALLBACK, disable_enable_fuzzer_range, "Enable fuzzers specified in range", "1,2,mincore,..." },
//  { "exit-condition",  'e', 0,                    G_OPTION_ARG_FILENAME, xxx,                         "Program that indicates stop condition", NULL },
    { "list",              0, G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK, list_fuzzer_names,           "List all registered fuzzers", NULL },
    { NULL },
};

int main(int argc, char **argv)
{
    GTimer            *timer       = NULL;
    GOptionContext    *context     = NULL;
    glong              returncode  = 0;
    struct passwd     *user        = getpwnam("nobody");

    // Parse commandline.
    context = g_option_context_new("");

    // Install parameters.
    g_option_context_add_main_entries(context, parameters, NULL);

    if (g_option_context_parse(context, &argc, &argv, NULL) == false) {
        g_warning("Failed to parse command line arguments.");
        return 1;
    }

    // Set userids to zero, and create a new process group.
    setresuid(0, 0, 0);
    setpgrp();

    // Start the http daemon listening for status output.
    // TODO: Choose a random port and print a URL.
    MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION,
                     8080,
                     httpd_connect_policy,
                     NULL,
                     httpd_access_handler,
                     NULL,
                     MHD_OPTION_END);

    g_message("Open http://localhost:8080/status for status information");

    while (true) {
        if (get_process_count() <= 4 && fork() == 0) {
            g_message("master process forking off a new one");
            break;
        } else {
            kill(-getpid(), SIGCONT);
        }
        sleep(1);
    }

    // At this point, shared memory segments have been attached and the web
    // server is listening. We can drop privileges and become an unprivileged
    // user.

    // Drop all supplementary groups.
    if (setgroups(0, NULL) != 0) {
        g_error("unable to drop supplementary groups, %m");
        abort();
    }

    // Change to an unprivileged user and group.
    if (setresgid(user->pw_gid, user->pw_gid, user->pw_gid) != 0) {
        g_error("unable change group id, %m");
        abort();
    }

    if (setresuid(user->pw_uid, user->pw_uid, user->pw_uid) != 0) {
        g_error("unable to change user id, %m");
        abort();
    }

    // Warn user this might be dangerous.
    if (skip_danger_warning == false) {
        print_danger_warning();
    }

    // Used for timing fuzzers.
    timer = g_timer_new();

    while (true) {
        // Select a random fuzzer.
        syscall_fuzzer_t *fuzzer = &system_call_fuzzers[
            g_random_int_range(0, MAX_SYSCALL_NUM)
        ];

        // Skip if undefined or disabled.
        if (fuzzer->callback == NULL || fuzzer->flags & SYS_DISABLED) {
            continue;
        }

        //g_message("fuzzer %s selected, %u total executions", fuzzer->name, fuzzer->total);

        // Execute the fuzzer, timing the operation.
        g_timer_start(timer);

        // Fuzzers are executed in their own lwp, in order to isolate us from damage.
        returncode = fuzzer->callback(fuzzer);

        // Terminate timer.
        g_timer_stop(timer);

        // Keep a running average of speed for this fuzzer.
        fuzzer->average = ((fuzzer->average * fuzzer->total) + g_timer_elapsed(timer, NULL))
                            / (fuzzer->total + 1);

        // And keep track of executions.
        fuzzer->total++;

        //g_message("fuzzer %s executed in %f seconds, returned %d (%s)",
        //          fuzzer->name,
        //          g_timer_elapsed(timer, NULL),
        //          returncode,
        //          g_strerror(returncode));

        // Should I ignore this?
        if (fuzzer->flags & SYS_VOID) {
            returncode = ESUCCESS;
        }

        // Is this supposed to fail?
        if (fuzzer->flags & SYS_FAIL && returncode == ESUCCESS) {
            g_critical("fuzzer %s unexpectedly succeeded", fuzzer->name);
            abort();
        }

        // Record error distribution to spot poor coverage.
        if (returncode != ESUCCESS) {
            error_record_t *error, key = { returncode, 0 };

            // Make sure this looks sane.
            g_assert_cmpuint(fuzzer->numerrors, <, MAX_ERROR_CODES);
            g_assert_cmpuint(fuzzer->failures, <, fuzzer->total);

            // Define compare callback for lsearch().
            gint compare_error(gconstpointer a, gconstpointer b)
            {
                return ((const error_record_t *)(a))->error
                    -  ((const error_record_t *)(b))->error;
            }

            // XXX: if this is a bottleneck, qsort on insertion and use bsearch().
            error = lsearch(&key,                   // key
                            fuzzer->errors,         // base
                            &fuzzer->numerrors,     // num
                            sizeof key,             // size
                            compare_error);         // compare

            // I don't expect this routine to fail.
            g_assert(error);

            // Record this error.
            fuzzer->failures++;

            // Check if it's new.
            if (error->count++ == 0) {
                g_message("fuzzer %s returned a new error, %s (%u executions, %u failures).",
                          fuzzer->name,
                          g_strerror(error->error),
                          fuzzer->total,
                          fuzzer->failures);
            }

            //if (fuzzer->total > 1024) {
            //  if (fuzzer->failures == fuzzer->total && fuzzer->numerrors == 1) {
            //      g_message("disabled boring fuzzer %s", fuzzer->name);
            //      fuzzer->flags |= SYS_DISABLED;
            //    }
            //}
        }
    }

    g_timer_destroy(timer);

    return 0;
}

// Users are allowed to disable ranges of fuzzers via the command line, this
// GOptionArgFunc handles one of those ranges. The intended purpose of this
// routine is to allow users to bisect a crash or unusual behaviour, disabling
// as many fuzzers as possible until they have a minimised set.
//
// Hopefully this will make debugging easier as well.
//
// Examples:
//
//  --disable 1,2,3-12,82                       // System call numbers
//  --disable read,write,1,3,4-9,exit           // Mixed names and numbers
//  --disable mq*,12                            // Globbing supported
//
// TODO: This routine also handles enabling, so you can do:
//
//  --disable * --enable 32
//  --disable 0-32 --enable 8
//
// And so on. I need to look at option_name to decide what to do.
static gboolean disable_enable_fuzzer_range(const gchar *option_name, const gchar *value, gpointer data, GError **error)
{
    guint     sysno     = 0;
    guint     max       = 0;
    gchar   **ranges    = NULL;
    gchar    *endptr    = NULL;
    gboolean  enable    = false;

    // Should I be enabling or disabling the specified fuzzers? The 2 is to
    // skip over the "--" prefix. I do not support short options for this.
    enable  = g_strcmp0(option_name + 2, "enable")
                ? false
                : true;

    // Split the argument by comma, our delimiter.
    ranges  = g_strsplit(value, ",", -1);

    // Now that each specifier has been split out, process each one.
    for (guint i = 0; i < g_strv_length(ranges); i++) {

        // Test the first character to decide what we should do with this.
        switch (ranges[i][0]) {

            // A system call number, or number range to disable, valid
            // specifications are either N, or N-M. Example valid constructs
            // might be '1', or '2-3'.
            case '0' ... '9':
                // Parse the first number.
                sysno   = g_ascii_strtoll(ranges[i], &endptr, 10);

                // FIXME: this shouldnt be an assert.
                g_assert_cmpint(sysno, <, MAX_SYSCALL_NUM);

                // Decide what we should do based on where parsing stopped.
                switch (*endptr) {
                    // End of string, there was just a single number, simply
                    // disable this fuzzer and break.
                    case '\0':
                        if (enable) {
                            system_call_fuzzers[sysno].flags &= ~SYS_DISABLED;
                        } else {
                            system_call_fuzzers[sysno].flags |= SYS_DISABLED;
                        }

                        g_debug("System call %s was %s as it matched range %s.",
                                system_call_fuzzers[sysno].name,
                                enable ? "enabled" : "disabled",
                                ranges[i]);

                        break;

                    // A dash, this was the first number of a range.
                    case  '-':
                        // Increment past the dash.
                        endptr++;

                        // Parse out the next number.
                        max = g_ascii_strtoll(endptr, &endptr, 10);

                        // FIXME: make these real checks.
                        g_assert_cmpint(sysno, <=, max);
                        g_assert_cmpint(max, >=, 0);
                        g_assert_cmpint(max, <, MAX_SYSCALL_NUM);
                        g_assert_cmpint(*endptr, ==, 0);

                        // Now disable every fuzzer in the range.
                        for (sysno = sysno; sysno <= max; sysno++) {
                            if (enable) {
                                system_call_fuzzers[sysno].flags &= ~SYS_DISABLED;
                            } else {
                                system_call_fuzzers[sysno].flags |= SYS_DISABLED;
                            }

                            g_debug("System call %s was %s as it matched range %s.",
                                    system_call_fuzzers[sysno].name,
                                    enable ? "enabled" : "disabled",
                                    ranges[i]);
                        }

                        break;

                    // Anything else must be a syntax error.
                    default: g_warning("System call specification %s unrecognised, gave up parsing at %s.",
                                       ranges[i],
                                       endptr);
                              goto error;
                }
                break;

            // Any other character in dicates a name glob, which is matched against all
            // system call names known. An example might be 'mq_*' to match
            // against the message queue system calls, like mq_open, mq_close,
            // etc.
            default:
                // For every systemcall, see if this is a match.
                for (sysno = 0; sysno < MAX_SYSCALL_NUM; sysno++) {

                    // Check if it has a name defined we can match.
                    if (system_call_fuzzers[sysno].name == NULL)
                        continue;

                    // Check if this syscall matches the glob specified.
                    if (g_pattern_match_simple(ranges[i], system_call_fuzzers[sysno].name)) {
                        g_debug("%s fuzzer %s, as it matches glob %s specified.",
                                enable ? "Enabling" : "Disabling",
                                system_call_fuzzers[sysno].name,
                                ranges[i]);

                        // Set or unset the SYS_DISABLED flag.
                        if (enable) {
                            system_call_fuzzers[sysno].flags &= ~SYS_DISABLED;
                        } else {
                            system_call_fuzzers[sysno].flags |=  SYS_DISABLED;
                        }
                    }
                }
                break;
        }
    }

    g_strfreev(ranges);
    return true;

error:
    g_strfreev(ranges);
    return false;
}

// Option callback to pretty print all registered fuzzers.
// Output looks like this:
//
// / Num / Name             / D / F / T / V / B / S /
// |  0  | restart_syscall  | 1 |   |  |  1 | 1 | 1 |
// |
// ...
static gboolean list_fuzzer_names(const gchar *option_name, const gchar *value, gpointer data, GError **error)
{

    // Print table header.
    g_print("/ Num / Name                           / D / F / T / V / B / S /\n");

    // Enumerate all system calls.
    for (guint i = 0; i < MAX_SYSCALL_NUM; i++) {
        // Check that a fuzzer exists.
        if (system_call_fuzzers[i].name == NULL) {
            g_debug("No fuzzer defined for systemcall %u", i);
            continue;
        }

        // Pretty print it.
        g_print("| %3u | %-30s | %c | %c | %c | %c | %c | %c |\n",
                 i,
                 system_call_fuzzers[i].name,
                 system_call_fuzzers[i].flags & SYS_DISABLED ? 'Y' : ' ',
                 system_call_fuzzers[i].flags & SYS_FAIL     ? 'Y' : ' ',
                 system_call_fuzzers[i].flags & SYS_TIMEOUT  ? 'Y' : ' ',
                 system_call_fuzzers[i].flags & SYS_VOID     ? 'Y' : ' ',
                 system_call_fuzzers[i].flags & SYS_BORING   ? 'Y' : ' ',
                 system_call_fuzzers[i].flags & SYS_SAFE     ? 'Y' : ' ');
    }

    // No need to continue, this is similar to asking for --help.
    exit(EXIT_SUCCESS);
}

// Show a warning about what user is about to do, this can be disabled at
// runtime via --dangerous.
static void print_danger_warning(void)
{
    const gint NumSecondsDelay = 10;

    g_warning("You can avoid this warning in future by specifying `--dangerous` on the commandline.");

    g_print("\n\n\n"
            "*********************************** WARNING ************************************\n"
            "* This program is dangerous, and will deliberately try to break your system.   *\n"
            "* Any writable files may be modified or unlinked, or a system crash may be     *\n"
            "* caused, resulting in filesystem corruption.                                  *\n"
            "*                                                                              *\n"
            "* This program is intended to be used on isolated test or virtualised systems, *\n"
            "* as an unprivileged user. Make sure any nfs or hgfs mounts are intended.      *\n"
            "*                                                                              *\n"
            "* I will sleep for %3u seconds before continuing. Interrupt me now if this is  *\n"
            "* not what you want.                                                           *\n"
            "********************************************************************************\n\n\n\a",
            NumSecondsDelay);

    // Give the user a chance to cancel.
    for (gint i = 1; i <= NumSecondsDelay; i++) {
        g_print("%d...\a", i); sleep(1);
    }

    g_print("\n");

    return;
}

// Callback from HTTP server, send it report data.
static gint httpd_access_handler(gpointer cls,
                                 struct MHD_Connection *connection,
                                 const gchar *url,
                                 const gchar *method,
                                 const gchar *version,
                                 const gchar *upload_data,
                                 gsize *upload_data_size,
                                 gpointer *con_cls)
{
    CSPARSE             *parse      = NULL;
    HDF                 *hdf        = NULL;
    gchar               *output     = NULL;
    struct MHD_Response *response   = NULL;

    // Create a callback for clearsilver output.
    NEOERR * get_clearsilver_output(gpointer ctx, gchar *string)
    {
        // Append the string output.
        gchar *combined = g_strconcat(output ? output : "", string, NULL);

        // Clean up.
        g_free(output);

        output = combined;

        return STATUS_OK;
    }

    // Initialise clearsilver.
    hdf_init(&hdf);

    // Generate a report in HDF format.
    create_fuzzer_report(hdf);

    // Parse the template file.
    g_assert(cs_init(&parse, hdf) == STATUS_OK);
    g_assert(cs_parse_file(parse, "report/main.cs") == STATUS_OK);
    g_assert(cs_render(parse, NULL, get_clearsilver_output) == STATUS_OK);

    // Create the response data using clearsilver.
    response =  MHD_create_response_from_data(strlen(output),
                                              output,
                                              MHD_NO,
                                              MHD_NO);

    MHD_queue_response(connection,
                       MHD_HTTP_OK,
                       response);

    MHD_destroy_response(response);

    cs_destroy(&parse);
    hdf_destroy(&hdf);
    return MHD_YES;
}

// TODO: Add some command line options to limit who is permitted to connect.
static gint httpd_connect_policy(gpointer cls, const struct sockaddr *addr, socklen_t addrlen)
{
    return MHD_YES;
}

// We use a sysv semaphore to protect access to shared structures. These are
// kernel backed, so we get things like killed processes holding locks taken
// care of for free using SEM_UNDO. But happens when a process takes the lock,
// and then takes a SIGSTOP?
//
// We need a watchdog process that verified the lock is available every few
// seconds, if it isn't, we just take down whoever held it and let the kernel
// clean up.
static void create_watchdog_process(void)
{
    // Let parent continue, watchdog child takes over.
    if (fork() != 0) {
        return;
    }

    setpgrp();

    g_message("watchdog process %lu started, attach count is %u", syscall(__NR_getpid), get_process_count());

    // Try to take the lock continually.
    while (get_process_count() > 1) {
        struct timespec timeout = {
            .tv_sec     = 0,
            .tv_nsec    = 500000000UL,
        };

        if (semtimedop(semid, takelock, G_N_ELEMENTS(takelock), &timeout) != 0) {
            switch (errno) {
                case EAGAIN: {
                    // Timeout, find out who has the lock.
                    gint lockholder = semctl(semid, 0, GETPID);

                    g_message("watchdog detected deadlock, lock holder is process %u", lockholder);

                    // See if we can coax him into letting go.
                    kill(lockholder, SIGKILL);

                    // Release the lock, even if he's in D, when he wakes up he will just die.
                    break;
                }
                // EINTR is harmless, just try again.
                case EINTR:
                    g_message("watchdog was interrupted");
                    continue;

                default:
                    g_error("watchdog encountered unexpected error from semop, %m");
                    abort();
            }
        }

        g_message("watchdog process took the lock %u, releasing it...", get_process_count());

        if (semop(semid, releaselock, G_N_ELEMENTS(releaselock)) != 0) {
            g_error("watchdog process failed to release the lock, %m");
            abort();
        }

        sleep(1);
    }

    g_error("watchdog process giving up, parent process appears to have died");
}
