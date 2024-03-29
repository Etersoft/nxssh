/**************************************************************************/
/*                                                                        */
/* Copyright (c) 2001, 2011 NoMachine, http://www.nomachine.com/.         */
/*                                                                        */
/* NXSSH, NX protocol compression and NX extensions to this software      */
/* are copyright of NoMachine. Redistribution and use of the present      */
/* software is allowed according to terms specified in the file LICENSE   */
/* which comes in the source distribution.                                */
/*                                                                        */
/* Check http://www.nomachine.com/licensing.html for applicability.       */
/*                                                                        */
/* NX and NoMachine are trademarks of Medialogic S.p.A.                   */
/*                                                                        */
/* All rights reserved.                                                   */
/*                                                                        */
/**************************************************************************/

#include "includes.h"
#include "log.h"
#include "openbsd-compat/sys-queue.h"
#include "channels.h"
#include "xmalloc.h"

/*
 * Used in NX network related functions.
 */

#include "NX.h"
#include "proxy.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <ctype.h>

#include <signal.h>
#include <netdb.h>

#if defined(__CYGWIN32__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__sun)
#include <netinet/in_systm.h>
#endif

#include <netinet/in.h>

#ifdef __sun
#define INADDR_NONE  ((unsigned int) -1)
#endif

/*
 * Set here the requested log level.
 */

#define PANIC
#define WARNING
#undef  TEST
#undef  DEBUG

/*
 * Print a line in the log if the time we
 * spent inside the select or handling the
 * messages exceeded a given time value.
 */

#undef  TIME

/*
 * The NX version we are advertising to the proxy.
 */

#define NX_ADVERTISE_VERSION "3.0.0"

/*
 * Replace stdin and stdout upon the switch. This
 * is not supported yet.
 */

#undef NX_REPLACE_STANDARD_DESCRIPTORS

/*
 * Close selectively the read and write ends of
 * the proxy and channel pipes in the proxy loop.
 * This is just for reference as it is better to
 * close all the descriptors at the first failure.
 */

#undef NX_SELECTIVELY_CLOSE_DESCRIPTORS

/*
 * Use a well-known log file and share it with the
 * proxy when the program is forwarding a SSHD con-
 * nection to the agent. To be used only for test
 * purposes.
 */

#undef NX_SHARE_BINDER_LOG

/*
 * Do we check the standard input for the incoming
 * switch command? This flag is bound to a command
 * line parameter.
 */

int nx_check_switch = 0;

/*
 * Did we receive the command? The switch is executed
 * at the time the first channel receives the command,
 * hence the port forwarding feature of SSH should be
 * disabled when enabling the NX proxying. This is
 * required to ensure that only the standard input has
 * the possibility to perform the switch. Monitoring
 * multiple concurrent channels is not implemented and
 * would be of little use in the NX context.
 *
 * This is the typical layout of the file descriptors
 * after the two proxies are connected through SSH:
 *
 * +-------+            +-------+
 * |       |            |       |
 * |   X   |<---[12]--->| Proxy |<---[8,7]---> ...
 * |       |            |       |
 * +-------+            +-------+
 *
 *                              +-------+
 * ... <---[8,7]---|---[4]-in-->|       |
 *                 |<--[5]-out--|  SSH  |<--[3]--/
 *                  +--[6]-err--|       |       /-----> SSHD
 *                  |           +-------+
 *                 file
 */

int nx_switch_received = 0;

/*
 * Do we redirect the debug log after the switch
 * command? If so, we'll try to determine what is the
 * log file used by the proxy and will append the out-
 * put to the same log. This is useful when debugging
 * the communication between nxssh and the NX trans-
 * port. If the proxy log can't be determined, for
 * example because the NX transport is not in use, the
 * output will go to a well known file, but only if
 * the NX_SHARE_BINDER_LOG is defined. This is useful
 * when debugging the nxssh forwarding of the SSHD
 * connection to the agent, as you can force the proxy
 * to use the same file. For this to work, the file
 * must be made writable by everybody. Beware that
 * normally this forwarding process is run by nxserver
 * as the nx user.
 */

int nx_switch_log = 0;

/*
 * Parameters read from the switch command.
 */

char nx_switch_cookie[256]   = { 0 };
char nx_switch_host[256]     = { 0 };
int  nx_switch_proxy         = -1;
int  nx_switch_port          = -1;
int  nx_switch_in            = -1;
int  nx_switch_out           = -1;
char nx_switch_mode[256]     = { 0 };
char nx_switch_options[1024] = { 0 };
int  nx_switch_internal      = -1;
int  nx_switch_forward       = -1;

/*
 * The awaited switch command.
 */

const char *nx_switch_command = "NX> 299 Switch connection to: ";

/*
 * Buffer the input while looking for the command.
 * The buffering happens by flushing the input when
 * a newline is received. This means that all input
 * should be terminated with a newline or it will
 * remain in the buffer and will never be sent to
 * the packet interface.
 */

static int nx_check_input(Buffer *buffer, char *data, int *length, int limit);

static int nx_check_switch_command(Buffer *buffer, char *start);
static int nx_check_switch_parameters(char *command);

/*
 * Parse the parameters from the switch command.
 */

static int nx_check_specifier_mode_and_options(char *command, char *mode, char *options);
static int nx_check_specifier_and_mode(char *command, char *mode);
static int nx_check_specifier_and_options(char *command, char *options);
static int nx_check_specifier(char *command);
static int nx_check_host_and_port(char *command, char *host, char *port);
static int nx_check_host_port_and_cookie(char *command, char *host, char *port, char *cookie);
static int nx_check_host_port_and_descriptors(char *command, char *host, char *port, char *in, char *out);
static int nx_check_port_and_accept(char *command, char *port, char *accept);
static int nx_check_descriptors(char *command, char *in, char *out);

/*
 * Connect to the proxy over a socketpair or
 * through a network connection.
 */

static int nx_open_internal_proxy_connection();
static int nx_open_external_proxy_connection();

/*
 * Redirect the log output.
 */

static void nx_redirect_log_output();

/*
 * Forward data read from the SSHD server to an
 * agent running on the NX server side.
 */

void nx_run_server_side_loop(int proxy_fd);

/*
 * Run a simple loop that lets the NX proxy on
 * the client connect to an agent running on
 * the NX server and make is manage the unenc-
 * rypted connection.
 */

void nx_run_client_side_loop(int proxy_fd);

/*
 * Buffers the standard input when waiting for the
 * command without having established any channel.
 */

static Buffer nx_input_buffer;

/*
 * Utilities to log in the NX format.
 */

static void nx_dump_buffer(Buffer *buffer);
static void nx_dump_string(char *string);

#if defined(DEBUG) || defined(TIME)

static char *nx_dump_timestamp();

static struct timeval nx_get_timestamp();
static int nx_diff_timestamp(struct timeval ts1, struct timeval ts2);

#endif

/*
 * Catch signals used to interrupt the connect or
 * detect a broken connection.
 */

static void nx_catch_timeout_signal(int number);
static void nx_catch_pipe_signal(int number);

/*
 * Safe version of string functions. They are able
 * to operate on strings delimited by a newline or
 * a null.
 */

static char *nx_search_string_in_buffer(Buffer *buffer, char *start, const char *string);
static char *nx_remove_string_in_buffer(Buffer *buffer, char *start, int length);
static char *nx_search_newline_in_buffer(Buffer *buffer, char *start);
static char *nx_remove_newline_in_string(char *start, int length);
static char *nx_toupper_string(char *start);

/*
 * This is currently unused. Not defined
 * static to avoid the warning.
 */

char *nx_search_char_in_buffer(Buffer *buffer, char *start, int value);

/*
 * Wait for the given amount of seconds.
 */

static void nx_wait_timeout(int seconds);

/*
 * Utility used to set our preferred socket
 * options. May alternatively use the SSH
 * functions in misc.c.
 */

static int nx_set_nonblocking(int fd);
static int nx_set_blocking(int fd);
static int nx_set_nodelay(int fd);
static int nx_set_keepalive(int fd);
static int nx_set_lowdelay(int fd);

void nx_proxy_init()
{
    buffer_init((&nx_input_buffer));
}

int nx_proxy_select(int maxfds, fd_set *readfds, fd_set *writefds,
                        fd_set *exceptfds, struct timeval *timeout)
{
        /*
         * Check if we have switched the connection
         * to the NX transport.
         */

        if (nx_switch_internal == 1 && NXTransRunning(NX_FD_ANY) == 1)
        {
                fd_set t_readfds, t_writefds;
                struct timeval t_timeout;

                int n, r, e;

                if (exceptfds != NULL)
                {
                        fatal("NX> 280 Can't handle exception fds in select");

                        exit(1);
                }

                #ifdef TEST
                debug("NX> 280 Going to run a new NX loop");
                #endif

                if (readfds == NULL)
                {
                        FD_ZERO(&t_readfds);

                        readfds = &t_readfds;
                }

                if (writefds == NULL)
                {
                        FD_ZERO(&t_writefds);

                        writefds = &t_writefds;
                }

                if (timeout == NULL)
                {
                        t_timeout.tv_sec  = 10;
                        t_timeout.tv_usec = 0;

                        timeout = &t_timeout;
                }

                n = maxfds;

                /*
                 * If the transport is gone avoid
                 * sleeping until the timeout.
                 */

                if (NXTransPrepare(&n, readfds, writefds, timeout) != 0)
                {
                        /*
                         * Merge the SSH descriptors with the NX
                         * descriptors and give a chance to the
                         * proxy to run its own loop.
                         */

                        NXTransSelect(&r, &e, &n, readfds, writefds, timeout);

                        NXTransExecute(&r, &e, &n, readfds, writefds, timeout);

                        errno = e;

                        return r;
                }
                else
                {
                        return 0;
                }
        }
        else
        {
                #ifdef DEBUG
                debug("NX> 280 The NX transport is not running");
                #endif

                return select(maxfds, readfds, writefds, exceptfds, timeout);
        }
}

int nx_check_channel_input(Channel *channel, char *data, int *length, int limit)
{
        debug("NX> 285 Going to check input for fd: %d", channel->rfd);

        /*
         * This allows forwarding of connections to
         * the authentication agent but it is to be
         * better investigated if this is the best
         * way to proceed.
         */

        if (strcmp(channel->ctype, "authentication agent connection") == 0)
        {
                debug("NX> 285 Detected authentication agent connection");

                return 0;
        }

        /*
         * It returns true if more data has been
         * produced for the channel. The switch
         * command is removed from the buffer.
         */

        return nx_check_input(&channel->nx_buffer, data, length, limit);
}

int nx_check_standard_input()
{
        char data[1024];

        fd_set set;

        int fd = fileno(stdin);

        int result;

        debug("NX> 285 Going to check input for fd: %d", fd);

        FD_ZERO(&set);

        for (;;)
        {
                FD_SET(fd, &set);

                result = select(fd + 1, &set, NULL, NULL, NULL);

                if (result < 0 && errno == EINTR)
                {
                        continue;
                }

                if (result <= 0)
                {
                        goto nx_check_standard_input_error;
                }

                for (;;)
                {
                        result = read(fd, data, 1024 - 1);

                        if (result < 0 && errno == EINTR)
                        {
                                continue;
                        }

                        if (result <= 0)
                        {
                                goto nx_check_standard_input_error;
                        }

                        /*
                         * No echo performed. Just print a warning.
                         * Actually we should only receive the com-
                         * mand. Any other data is discarded.
                         */

                        nx_check_input(&nx_input_buffer, data, &result, 1024 - 1);

                        if (result > 0)
                        {
                                *(data + result) = '\0';

                                nx_remove_newline_in_string(data, result);

                                error("NX> 289 Discarding spurious data: '%s'", data);

                                buffer_clear(&nx_input_buffer);
                        }

                        break;
                }

                break;
        }

        return 1;

nx_check_standard_input_error:

        fatal("NX> 290 End of input received before switch command");

        return -1;
}

int nx_check_input(Buffer *buffer, char *data, int *length, int limit)
{
        char *start;
        char *current;
        char *end;

        /*
         * Append data to the buffer.
         */

        if (buffer_len(buffer) + *length >= (16*1024 - 1))
        {
                error("\r\nNX> 288 Buffer length of: %d bytes exceeded",
                              16*1024);

                fatal("NX> 290 Don't use the -B option with binary data");
        }

        debug("NX> 280 Adding: %d bytes to the temporary buffer",
                      *length);

        buffer_append(buffer, data, *length);

        debug("NX> 280 Dumping content of the temporary buffer:");

        nx_dump_buffer(buffer);

        /*
         * Check if buffer contains the switch command.
         */

        start = nx_search_string_in_buffer(buffer, NULL, nx_switch_command);

        if (start != NULL)
        {
                debug("NX> 280 Switch command maybe found at position: %d",
                              (int) (start - (char *) buffer_ptr(buffer)));

                /*
                 * Verify the host and port and wipe out
                 * the command from the buffer.
                 */

                nx_switch_received = nx_check_switch_command(buffer, start);
        }

        /*
         * Place back the remaining data in
         * the original buffer.
         */

        current = data;
        *length = 0;

        for (;;)
        {
                start = buffer_ptr(buffer);

                end = nx_search_newline_in_buffer(buffer, start);

                if (end == NULL || *length + (end - start + 1) > limit)
                {
                        break;
                }

                debug("NX> 280 Adding: %d bytes to the returned buffer",
                             (int) (end - start + 1));

                buffer_get(buffer, current, end - start + 1);

                current += end - start + 1;
                *length += end - start + 1;
        }

        debug("NX> 280 Left: %d bytes in the temporary buffer",
                    buffer_len(buffer));

        return 1;
}

int nx_check_switch_command(Buffer *buffer, char *start)
{
        char *command;
        char *end;

        int length = buffer_len(buffer) - (start - (char *) buffer_ptr(buffer));

        debug("NX> 280 Searching newline with: %d bytes remaining", length);

        if ((end = nx_search_newline_in_buffer(buffer, start)) == NULL)
        {
                fatal("NX> 290 Can't find the terminating newline in buffer:");

                return 0;
        }

        /*
         * Copy the command string to a temporary.
         */

        length = end - start;

        debug("NX> 280 Checking command with length: %d", length);

        command = xmalloc(length + 1);

        if (command == NULL)
        {
                fatal("NX> 298 Out of memory checking the command string");

                return -1;
        }

        memcpy(command, start, length);

        *(command + length) = '\0';

        debug("NX> 280 Analyzing command string:");

        nx_dump_string(command);

        /*
         * Get rid of the command fingerprint from
         * buffer until the terminating newline.
         */

        nx_remove_string_in_buffer(buffer, start, length);

        debug("NX> 280 Content of the temporary buffer:");

        nx_dump_buffer(buffer);

        if (nx_check_switch_parameters(command) <= 0)
        {
                /*
                 * Skip the fingerprint part in the
                 * error message.
                 */

                fatal("\r\nNX> 298 Parse error in the switch parameters");

                return -1;
        }

        return 1;
}

int nx_check_switch_parameters(char *command)
{
        char host[256]     = { 0 };
        char accept[256]   = { 0 };
        char cookie[256]   = { 0 };
        char mode[256]     = { 0 };
        char options[1024] = { 0 };

        char port[16] = { 0 };
        char in[16]   = { 0 };
        char out[16]  = { 0 };

        if (command == NULL || *command == '\0')
        {
                return -1;
        }
        else if (strlen(command) >= 256)
        {
                return -1;
        }

        debug("NX> 285 Searching switch parameters in command:");

        nx_dump_string(command);

        if (nx_check_specifier_mode_and_options(command, mode, options) <= 0 &&
                nx_check_specifier_and_options(command, options) <= 0 &&
                    nx_check_specifier_and_mode(command, mode) <= 0 &&
                        nx_check_port_and_accept(command, port, accept) <= 0 &&
                                nx_check_host_port_and_descriptors(command, host, port, in, out) <= 0 &&
                                        nx_check_descriptors(command, in, out) <= 0 &&
                                            nx_check_host_port_and_cookie(command, host, port, cookie) <= 0 &&
                                                nx_check_host_and_port(command, host, port) <= 0 &&
                                                    nx_check_specifier(command) <= 0)
        {
                return -1;
        }

        /*
         * If at least host and port were given
         * we need an outbound connection.
         */

        if (*host != '\0' && *port != '\0')
        {
                strcpy(nx_switch_host, host);

                nx_switch_port = atoi(port);

                logit("NX> 285 Identified host: %s port: %d", nx_switch_host, nx_switch_port);

                if (*cookie != '\0')
                {
                        strcpy(nx_switch_cookie, cookie);

                        logit("NX> 285 Identified cookie: %s", nx_switch_cookie);
                }

                if (*in != '\0' && *out != '\0')
                {
                        nx_switch_in  = atoi(in);
                        nx_switch_out = atoi(out);

                        logit("NX> 285 Identified descriptors in: %d out: %d", nx_switch_in, nx_switch_out);
                }


                nx_switch_internal = 0;
        }
        else if (*in != '\0' && *out != '\0')
        {

                nx_switch_in  = atoi(in);
                nx_switch_out = atoi(out);

                logit("NX> 285 Identified descriptors in: %d out: %d", nx_switch_in, nx_switch_out);

                nx_switch_forward = 1;
        }
        else if (*accept != '\0' && *port != '\0')
        {
                strcpy(nx_switch_host, accept);
                nx_switch_port = atoi(port);

                logit("NX> 285 Identified port: %d accept: %s", nx_switch_port, nx_switch_host);

                nx_switch_forward = 2;
        }
        else
        {
                logit("NX> 285 Identified internal connection");

                if (*mode != '\0')
                {
                        /*
                         * The mode "encrypted" is assumed to be the
                         * default and is left to the original empty
                         * string. Unencrypted connections are run
                         * by entering a different loop.
                         */

                        if (strcmp("encrypted", mode) != 0 &&
                                    strcmp("unencrypted", mode) != 0 &&
                                            strcmp("default", mode) != 0)
                        {
                                error("NX> 290 Not supported mode: %s", mode);

                                return -1;
                        }

                        if (strcmp("unencrypted", mode) == 0)
                        {
                                strcpy(nx_switch_mode, mode);

                                logit("NX> 285 Identified mode: %s", nx_switch_mode);
                        }
                        else
                        {
                                logit("NX> 285 Using default mode encrypted");
                        }
                }

                if (*options != '\0')
                {
                        strcpy(nx_switch_options, options);

                        logit("NX> 285 Identified options: %s", nx_switch_options);
                }

                nx_switch_internal = 1;
        }

        return 1;
}

int nx_check_host_port_and_descriptors(char *command, char *host, char *port, char *in, char *out)
{
        char match[256]  = { 0 };

        int result;

        /*
         * Look for the hostname and port followed
         * by the input and output fd descriptors.
         */

        snprintf(match, 255, "%s%%128[^:]:%%5[0-9] in: %%5[0-9] out: %%5[0-9]", nx_switch_command);

        debug("NX> 280 Searching with matching string:");

        nx_dump_string(match);

        result = sscanf(command, match, host, port, in, out);

        if (result != 4)
        {
                return -1;
        }

        return 1;
}

int nx_check_port_and_accept(char *command, char *port, char *accept)
{
        char match[256]  = { 0 };

        int result;

        /*
         * Look for the port.
         */

        snprintf(match, 255, "%s SSH port: %%5[0-9] accept: %%128s", nx_switch_command);

        debug("NX> 280 Searching with matching string:");

        nx_dump_string(match);

        result = sscanf(command, match, port, accept);

        if (result != 2)
        {
                return -1;
        }

        return 1;
}

int nx_check_descriptors(char *command, char *in, char *out)
{
        char match[256]  = { 0 };

        int result;

        /*
         * Look for the input and output fd descriptors.
         */

        snprintf(match, 255, "%s SSH in: %%5[0-9] out: %%5[0-9]", nx_switch_command);

        debug("NX> 280 Searching with matching string:");

        nx_dump_string(match);

        result = sscanf(command, match, in, out);

        if (result != 2)
        {
                return -1;
        }

        return 1;
}

int nx_check_host_port_and_cookie(char *command, char *host, char *port, char *cookie)
{
        char match[256]  = { 0 };

        int result;

        /*
         * Look for the hostname and port followed
         * by the cookie specification.
         */

        snprintf(match, 255, "%s%%128[^:]:%%5[0-9] cookie: %%32s", nx_switch_command);

        debug("NX> 280 Searching with matching string:");

        nx_dump_string(match);

        result = sscanf(command, match, host, port, cookie);

        if (result != 3)
        {
                return -1;
        }

        return 1;
}

int nx_check_host_and_port(char *command, char *host, char *port)
{
        char match[256]  = { 0 };

        int result;

        /*
         * Look for the hostname and port.
         */

        snprintf(match, 255, "%s%%128[^:]:%%5[0-9]", nx_switch_command);

        debug("NX> 280 Searching with matching string:");

        nx_dump_string(match);

        result = sscanf(command, match, host, port);

        if (result != 2)
        {
                return -1;
        }

        return 1;
}

int nx_check_specifier_mode_and_options(char *command, char *mode, char *options)
{
        char match[256]  = { 0 };

        int result;

        /*
         * Look for the NX mode and options. Mode must be
         * one of "default", "encrypted" or "unencrypted".
         * Options are an arbitrary string that will have
         * to make sense for NX.
         */

        snprintf(match, 255, "%s NX mode: %%16s options: %%1023[^']", nx_switch_command);

        debug("NX> 280 Searching with matching string:");

        nx_dump_string(match);

        result = sscanf(command, match, mode, options);

        if (result != 2)
        {
                return -1;
        }

        return 1;
}

int nx_check_specifier_and_mode(char *command, char *mode)
{
        char match[256]  = { 0 };

        int result;

        /*
         * Look for the NX mode. Options are assumed
         * to be empty.
         */

        snprintf(match, 255, "%s NX mode: %%16s", nx_switch_command);

        debug("NX> 280 Searching with matching string:");

        nx_dump_string(match);

        result = sscanf(command, match, mode);

        if (result != 1)
        {
                return -1;
        }

        return 1;
}

int nx_check_specifier_and_options(char *command, char *options)
{
        char match[256]  = { 0 };

        int result;

        /*
         * Look for the NX options. Mode is assumed to be
         * the current default.
         */

        snprintf(match, 255, "%s NX options: %%1023s", nx_switch_command);

        debug("NX> 280 Searching with matching string:");

        nx_dump_string(match);

        result = sscanf(command, match, options);

        if (result != 1)
        {
                return -1;
        }

        return 1;
}

int nx_check_specifier(char *command)
{
        char match[256]  = { 0 };

        char nx[256]  = { 0 };

        int result;

        /*
         * Look for the NX string.
         */

        snprintf(match, 255, "%s%%2s", nx_switch_command);

        debug("NX> 280 Searching with matching string:");

        nx_dump_string(match);

        result = sscanf(command, match, nx);

        if (result != 1 || strcmp("NX", nx_toupper_string(nx)) != 0)
        {
                return -1;
        }

        return 1;
}

int nx_open_proxy_connection()
{
        if (*nx_switch_host != '\0' && nx_switch_port != -1)
        {
                return nx_open_external_proxy_connection();
        }
        else if (nx_switch_internal == 1)
        {
                return nx_open_internal_proxy_connection();
        }
        else
        {
                fatal("\r\nNX> 297 Missing data to perform the switch");

                return -1;
        }
}

int nx_close_proxy_connection()
{
        /*
         * Yield the control to the proxy until the NX
         * transport is gone.
         */

        debug("NX> 280 Switch proxy is: %d", nx_switch_proxy);

        if (nx_switch_proxy != -1)
        {
                debug("NX> 280 Waiting for the NX transport to terminate");

                NXTransDestroy(nx_switch_proxy);

                debug("NX> 280 NX transport successfully terminated");
        }

        return 1;
}

/*
 * Let the proxy create a socketpair and receive
 * back the end we'll use to communicate with it.
 */

int nx_open_internal_proxy_connection()
{
        char *proxy_options;

        int proxy_pair[2];

        /*
         * Options can be passed in the environment
         * by the client by using an empty "options"
         * switch parameter.
         */

        if (*nx_switch_options != '\0')
        {
                proxy_options = nx_switch_options;

                debug("NX> 280 Creating proxy with options: %s", nx_switch_options);
        }
        else
        {
                proxy_options = NULL;

                debug("NX> 280 Creating proxy with null options");
        }

        /*
         * Create a socketpair. We will use the
         * proxy_pair[0] at our end and will
         * pass the proxy_pair[1] to NX.
         */

        if (socketpair(PF_UNIX, SOCK_STREAM, 0, proxy_pair) < 0)
        {
                fatal("NX> 290 Failure creating the socket pair: %s",
                              strerror(errno));

                return -1;
        }

        /*
         * If connection has to continue unencrypted
         * reset the remote end. NX will create a new
         * connection based on the proxy options.
         */

        if (strcmp("unencrypted", nx_switch_mode) == 0)
        {
                close(proxy_pair[1]);

                proxy_pair[1] = NX_FD_ANY;
        }
        else
        {
                /*
                 * Set the preferred options on our end of the
                 * socket. In the case of an unencrypted NX
                 * connection, the local end is just used as a
                 * place-holder. We will close it at the time
                 * we'll start the client loop.
                 */

                nx_set_socket_options(proxy_pair[0], 0);
        }

        debug("NX> 280 Using local end: %d remote end: %d",
                      proxy_pair[0], proxy_pair[1]);


        if (NXTransCreate(proxy_pair[1], NX_MODE_SERVER, proxy_options) < 0)
        {
                fatal("NX> 290 Failed to create the internal connection");

                return -1;
        }

        logit("NX> 280 Proxy opened with local: %d remote: %d",
                  proxy_pair[0], proxy_pair[1]);

        /*
         * Client-side support for memory-to-memory transport is not
         * implemented in nxcomp, yet. This means that communication
         * between nxcomp and nxssh takes place through a Unix pipe.
         *
         * if (strcmp("encrypted", nx_switch_mode) == 0)
         * {
         *         NXTransAgent(proxy_pair);
         * }
         */

        debug("NX> 280 Should enable NX memory-to-memory agent transport");

        nx_switch_proxy = proxy_pair[1];

        return proxy_pair[0];
}

/*
 * Connect to an external proxy process.
 */

int nx_open_external_proxy_connection()
{
        int proxy_fd;

        int remote_ip_addr;

        void (*handler)(int) = signal(SIGALRM, nx_catch_timeout_signal);

        /*
         * Set how many times we retry to connect
         * to the remote host in case of failure?
         */

        int retry_connect = 4;

        struct sockaddr_in addr;

        struct hostent *host = gethostbyname(nx_switch_host);

        int flag = 1;

        int result;

        /*
         * We are reusing the timer. This is not
         * optimal. Save at least the old value.
         */

        int timer = alarm(0);

        logit("\r\nNX> 291 Connecting to: %s:%d",
                    nx_switch_host, nx_switch_port);

        if (host == NULL)
        {
                /*
                 * On some Unices gethostbyname() doesn't
                 * accept IP addresses, so try inet_addr.
                 */

                unsigned int address;

                address = (unsigned int) inet_addr(nx_switch_host);

                if (address == INADDR_NONE)
                {
                        fatal("\r\nNX> 297 Can't resolve address of host: %s",
                                  nx_switch_host);

                        goto nx_open_proxy_connection_error;
                }

                remote_ip_addr = (int) address;
        }
        else
        {
                remote_ip_addr = (*((int *) host -> h_addr_list[0]));
        }

        addr.sin_family = AF_INET;
        addr.sin_port = htons(nx_switch_port);
        addr.sin_addr.s_addr = remote_ip_addr;

        for (;;)
        {
                proxy_fd = socket(AF_INET, SOCK_STREAM, PF_UNSPEC);

                if (proxy_fd == -1)
                {
                        fatal("\r\nNX> 296 Can't create the connecting socket");

                        goto nx_open_proxy_connection_error;
                }
                else if (setsockopt(proxy_fd, SOL_SOCKET, SO_REUSEADDR,
                                 (char *) &flag, sizeof(flag)) < 0)
                {
                        fatal("\r\nNX> 295 Can't set the SO_REUSEADDR on socket");

                        goto nx_open_proxy_connection_error;
                }

                /*
                 * Ensure operation is timed out
                 * if there is a network problem.
                 */

                alarm(retry_connect);

                result = connect(proxy_fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in));

                if (result < 0)
                {
                        retry_connect--;

                        if (retry_connect > 0)
                        {
                                error("NX> 294 Connection to: %s:%d failed. Retrying",
                                           nx_switch_host, nx_switch_port);

                                close(proxy_fd);
                        }
                        else
                        {
                                fatal("NX> 290 Failed connection to: %s:%d",
                                           nx_switch_host, nx_switch_port);

                                goto nx_open_proxy_connection_error;
                        }

                        nx_wait_timeout(3);
                }
                else
                {
                        break;
                }
        }

        debug("NX> 294 Connected to proxy at: %s:%d",
                   nx_switch_host, nx_switch_port);

        signal(SIGALRM, handler);

        alarm(timer);

        /*
         * Set the preferred NX options on the socket.
         */

        nx_set_socket_options(proxy_fd, 0);

        return proxy_fd;

nx_open_proxy_connection_error:

        signal(SIGALRM, handler);

        alarm(timer);

        return -1;
}

int nx_check_proxy_authentication(int proxy_fd)
{
        if (*nx_switch_cookie != '\0')
        {
                char string[256];

                /*
                 * String must be terminated with a space.
                 */

                snprintf(string, 255, "NXSSH-%s cookie=%s ", NX_ADVERTISE_VERSION,
                             nx_toupper_string(nx_switch_cookie));

                /*
                 * This should not be required.
                 */

                *(string + 255) = '\0';

                logit("NX> 285 Sending authentication cookie: %s",
                          nx_switch_cookie);

                write(proxy_fd, string, strlen(string));

                return 1;
        }

        return 0;
}

int nx_switch_client_side_descriptors(Channel *channel, int proxy_fd)
{
        /*
         * This is executed on the NX client side to
         * reassign the SSH channel's descriptors to
         * the socket we opened with the proxy.
         */

        if (nx_switch_internal == 1 &&
                strcmp("unencrypted", nx_switch_mode) == 0)
        {
                /*
                 * Enter the client-side I/O loop.
                 */

                nx_check_switch = 0;

                nx_run_client_side_loop(proxy_fd);

                logit("NX> 285 Client side loop completed");

                exit(0);
        }
        else
        {
                /*
                 * This is executed for both "internal" and "normal"
                 * connections, the latter being connection obtained
                 * by attaching to a proxy listening on a TCP port.
                 * The "internal" connections will be managed in a
                 * special way, in clientloop.c:
                 */

                logit("NX> 285 Switching descriptors: %d and: %d to: %d",
                            channel->rfd, channel->wfd, proxy_fd);

                if (dup2(proxy_fd, channel->rfd) < 0 || dup2(proxy_fd, channel->wfd) < 0)
                {
                        fatal("\r\nNX> 292 Can't redirect I/O to channel descriptors");
                }

                close(proxy_fd);

                /*
                 * Set again the preferred NX options on the
                 * involved fds after the dup2() even if this
                 * should not be required.
                 */

                nx_set_socket_options(channel->rfd, 0);
                nx_set_socket_options(channel->wfd, 0);

                logit("\r\nNX> 287 Redirected I/O to channel descriptors");

                logit("NX> 280 Proxy in: %d out: %d transport in: %d out: %d",
                          channel->rfd, channel->wfd, nx_switch_proxy, nx_switch_proxy);

                /*
                 * If requested, redirect the debug output to the
                 * error log used by the proxy. This can be useful
                 * when debugging the interaction between nxssh and
                 * the proxy.
                 */

                if (nx_switch_log == 1)
                {
                        nx_redirect_log_output();
                }

                nx_check_switch = 0;

                return 1;
        }
}

void nx_switch_server_side_descriptors()
{
        /*
         * This is executed on the NX server side to
         * forward the proxy I/O to the SSH daemon,
         * where it will be encrypted. The standard
         * error is left untouched. Our error output
         * will go to the standard error we inherited
         * from the NX server.
         */

        int proxy_fd;

        proxy_fd = nx_open_proxy_connection();

        if (proxy_fd < 0)
        {
                fatal("NX> 290 Can't switch communication to: %s:%d",
                          nx_switch_host, nx_switch_port);
        }

        /*
         * Check if we must authenticate to the proxy.
         */

        nx_check_proxy_authentication(proxy_fd);

        /*
         * Redirect I/O to the provided descriptors.
         */

        if (nx_switch_in >= 0 && nx_switch_out >= 0)
        {
                /*
                 * This has not been tested yet.
                 */

                #ifdef NX_REPLACE_STANDARD_DESCRIPTORS

                if (dup2(nx_switch_in,  fileno(stdin)) < 0)
                {
                        fatal("NX> 290 Can't duplicate descriptor: %d to: %d error: '%s'",
                                  nx_switch_in, fileno(stdin), strerror(errno));
                }
                else
                {
                        logit("NX> 285 Duplicated descriptor: %d to: %d",
                                  nx_switch_in, fileno(stdin));
                }

                if (dup2(nx_switch_out, fileno(stdout)) < 0)
                {
                        fatal("NX> 290 Can't duplicate descriptor: %d to: %d error: '%s'",
                                  nx_switch_out, fileno(stdout), strerror(errno));
                }
                else
                {
                        logit("NX> 285 Duplicated descriptor: %d to: %d",
                                  nx_switch_out, fileno(stdout));
                }

                #endif
        }
        else
        {
                fatal("NX> 290 Can't switch communication to in: %d out: %d",
                              nx_switch_in, nx_switch_out);
        }

        /*
         * Enter the server-side I/O loop.
         */

        nx_check_switch = 0;

        nx_run_server_side_loop(proxy_fd);
}

int nx_switch_forward_descriptors(Channel *channel)
{
        /*
         * This is executed on the NX server side to
         * tie together two descriptors, the one of
         * the ssh connection from client and the one
         * of the ssh connection to the node.
         */

        if (nx_switch_in >= 0 && nx_switch_out >= 0)
        {
                if (dup2(nx_switch_in,  channel->rfd) < 0)
                {
                        fatal("NX> 290 Can't duplicate descriptor: %d to: %d error: '%s'",
                                  nx_switch_in, channel->rfd, strerror(errno));
                }
                else
                {
                        logit("NX> 285 Duplicated descriptor: %d to: %d",
                                  nx_switch_in, channel->rfd);
                }

                if (dup2(nx_switch_out, channel->wfd) < 0)
                {
                        fatal("NX> 290 Can't duplicate descriptor: %d to: %d error: '%s'",
                                  nx_switch_out, channel->wfd, strerror(errno));
                }
                else
                {
                        logit("NX> 285 Duplicated descriptor: %d to: %d",
                                  nx_switch_out, channel->wfd);
                }
        }
        else
        {
                fatal("NX> 290 Can't switch communication to in: %d out: %d",
                          nx_switch_in, nx_switch_out);
        }

        nx_check_switch = 0;

        return 1;
}

int nx_switch_forward_port(Channel *channel)
{
        /*
         * This is executed on the NX server side to
         * open a port and forward all data to the
         * ssh connection to the node.
         */

        int retry_accept = 4;

        int proxy_fd = -1;
        int new_fd   = -1;

        int flag = 1;

        int remote_ip_addr;
        struct sockaddr_in tcp_addr;

        struct hostent *host = gethostbyname(nx_switch_host);

        if (host == NULL)
        {
                /*
                 * On some Unices gethostbyname() doesn't
                 * accept IP addresses, so try inet_addr.
                 */

                unsigned int address;

                address = (unsigned int) inet_addr(nx_switch_host);

                if (address == INADDR_NONE)
                {
                        fatal("\r\nNX> 297 Can't resolve address of host: %s",
                                      nx_switch_host);

                        goto nx_switch_forward_port_error;
                }

                remote_ip_addr = (int) address;
        }
        else
        {
                remote_ip_addr = (*((int *) host -> h_addr_list[0]));
        }

        if (remote_ip_addr == 0)
        {
                fatal("\r\nNX> 297 Cannot accept connections from unknown host: %s",
                              nx_switch_host);

                goto nx_switch_forward_port_error;
        }

        proxy_fd = socket(AF_INET, SOCK_STREAM, PF_UNSPEC);

        if (proxy_fd == -1)
        {

                fatal("\r\nNX> 297 Can't create socket error: %s",
                              strerror(errno));

                goto nx_switch_forward_port_error;
        }
        else if (setsockopt(proxy_fd, SOL_SOCKET, SO_REUSEADDR,
                         (char *) &flag, sizeof(flag)) < 0)
        {
                fatal("\r\nNX> 295 Can't set the SO_REUSEADDR on socket");

                goto nx_switch_forward_port_error;
        }

        tcp_addr.sin_family = AF_INET;
        tcp_addr.sin_port = htons(nx_switch_port);
        tcp_addr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(proxy_fd, (struct sockaddr *) &tcp_addr, sizeof(tcp_addr)) == -1)
        {
                fatal("\r\nNX> 297 Can't bind to port: %i error: %s",
                              nx_switch_port, strerror(errno));

                goto nx_switch_forward_port_error;
        }

        if (listen(proxy_fd, 4) == -1)
        {
                fatal("\r\nNX> 297 Can't listen on port: %i error: %s",
                              nx_switch_port, strerror(errno));

                goto nx_switch_forward_port_error;
        }

        logit("NX> 291 Waiting for remote connection on port: %d from: %s",
                  nx_switch_port, nx_switch_host);

        for (;;)
        {
                fd_set readSet;
                int result;

                struct timeval selectTs;


                FD_ZERO(&readSet);
                FD_SET(proxy_fd, &readSet);

                selectTs.tv_sec  = 20;
                selectTs.tv_usec = 0;

                result = select(proxy_fd + 1, &readSet, NULL, NULL, &selectTs);

                if (result == -1)
                {
                        /*
                         * Don't restart the call on a interrupt.
                         * It is likely that the server wants us
                         * to terminate the procedure.
                         *
                         * if (errno == EINTR)
                         * {
                         *         continue;
                         * }
                         */

                        fatal("\r\nNX> 297 Call to select failed error: %s",
                                      strerror(errno));

                        goto nx_switch_forward_port_error;
                }
                else if (result > 0 && FD_ISSET(proxy_fd, &readSet))
                {
                        struct sockaddr_in newAddr;
                        char *connected_host = inet_ntoa(newAddr.sin_addr);
                        unsigned int connected_port = ntohs(newAddr.sin_port);

                        socklen_t addrLen = sizeof(struct sockaddr_in);

                        new_fd = accept(proxy_fd, (struct sockaddr *) &newAddr, &addrLen);

                        if (new_fd == -1)
                        {
                                fatal("\r\nNX> 297 Call to accept failed error: %s",
                                          strerror(errno));

                                goto nx_switch_forward_port_error;
                        }

                        if ((int) newAddr.sin_addr.s_addr == remote_ip_addr)
                        {
                                #ifdef TEST
                                logit("NX> 291 Accepted connection from: %s with port: %d",
                                          connected_host, connected_port);
                                #endif

                                break;
                        }
                        else
                        {
                                fatal("NX> 297 Refused connection from: %s with port: %d",
                                          connected_host, connected_port);

                                goto nx_switch_forward_port_error;
                        }

                        /*
                         * Not the best way to elude a DOS attack.
                         */

                        sleep(5);

                        close(new_fd);
                }

                if (--retry_accept == 0)
                {

                        fatal("\r\nNX> 297 Connection with remote host: %s could not be established",
                                      nx_switch_host);

                        goto nx_switch_forward_port_error;
                }
        }

        close(proxy_fd);

        nx_set_socket_options(new_fd, 0);

        if (dup2(new_fd, channel->rfd) < 0 || dup2(new_fd, channel->wfd) < 0)
        {
                fatal("\r\nNX> 297 Can't redirect port to channel descriptors");
        }

        nx_check_switch = 0;

        return 1;

nx_switch_forward_port_error:

        close(proxy_fd);

        return -1;
}

void nx_run_server_side_loop(int proxy_fd)
{
        /*
         * This loop is run on the NX server side when the program
         * is run with the option -B. The loop just copies from the
         * standard input to the proxy descriptor and from the proxy
         * descriptor to to the standard output. The standard input
         * and the standard output of the process are connected by
         * the NX server to the SSHD's descriptors, so what this
         * loop does is actually to connect the client side proxy
         * to an agent running on the NX server.
         */

        char data[64 * 1024];

        fd_set set;

        int proxy_in  = dup(proxy_fd);
        int proxy_out = dup(proxy_fd);

        #ifdef NX_REPLACE_STANDARD_DESCRIPTORS

        int channel_in  = fileno(stdin);
        int channel_out = fileno(stdout);

        #else

        int channel_in  = nx_switch_in;
        int channel_out = nx_switch_out;

        #endif

        int proxy_in_open   = 1;
        int channel_in_open = 1;

        int in_fd;
        int out_fd;
        int failed_fd;

        int selected;
        int written;
        int length;

        int result;

        #if defined(DEBUG) || defined(TIME)

        struct timeval last_ts = nx_get_timestamp();

        int diff_ts;

        #endif

        /*
         * If requested, redirect the debug output.
         */

        if (nx_switch_log == 1)
        {
                nx_redirect_log_output();
        }

        close(proxy_fd);

        /*
         * Set the preferred NX options on all the
         * involved descriptors. We want blocking
         * operations on the sockets so that we can
         * rely on the NX proxy for the buffering
         * and minimize the context switches.
         */

        nx_set_socket_options(proxy_in, 1);
        nx_set_socket_options(proxy_out, 1);

        nx_set_socket_options(channel_in, 1);
        nx_set_socket_options(channel_out, 1);

        signal(SIGPIPE, nx_catch_pipe_signal);

        logit("NX> 285 Entering the server side NX loop with proxy at: %s:%d",
                     nx_switch_host, nx_switch_port);

        FD_ZERO(&set);

nx_run_server_side_loop_start:

        while (proxy_in_open || channel_in_open)
        {
                selected = 0;

                /*
                 * Put logging in #ifdef DEBUG to
                 * save the function calls.
                 */

                if (proxy_in_open)
                {
                        /*
                         * Debug output is printed with logit(),
                         * here, to override the settings that
                         * might have been passed in the config
                         * files.
                         */

                        #ifdef DEBUG
                        logit("NX> 280 Selecting proxy descriptor");
                        #endif

                        FD_SET(proxy_in, &set);

                        if (proxy_in >= selected)
                        {
                                selected = proxy_in + 1;
                        }

                }

                if (channel_in_open)
                {
                        #ifdef DEBUG
                        logit("NX> 280 Selecting channel descriptor");
                        #endif

                        FD_SET(channel_in, &set);

                        if (channel_in >= selected)
                        {
                                selected = channel_in + 1;
                        }
                }

                /*
                 * We could exit when the first end is gone,
                 * anyway we try to keep the link up to send
                 * the pending data that would eventually
                 * come.
                 */

                #if defined(DEBUG) || defined(TIME)

                diff_ts = nx_diff_timestamp(last_ts, nx_get_timestamp());

                #ifdef TIME

                if (diff_ts > 20)
                {
                        logit("NX> 280 TIME! Spent: %d ms handling messages",
                                  diff_ts);
                }

                #endif

                last_ts = nx_get_timestamp();

                logit("NX> 280 Entering select at: %s", nx_dump_timestamp());

                #endif

                selected = select(selected, &set, NULL, NULL, NULL);

                #if defined(DEBUG) || defined(TIME)

                diff_ts = nx_diff_timestamp(last_ts, nx_get_timestamp());

                logit("NX> 280 Out of select with result: %d at: %s after: %d ms",
                          selected, nx_dump_timestamp(), diff_ts);

                #ifdef TIME

                if (diff_ts > 20)
                {
                        logit("NX> 280 TIME! Spent: %d ms waiting for data",
                                  diff_ts);
                }

                #endif

                last_ts = nx_get_timestamp();

                #endif

                if (selected <= 0)
                {
                        if (selected < 0)
                        {
                                #ifdef DEBUG
                                logit("NX> 280 Got error: %d in select: '%s' at: %s",
                                          errno, strerror(errno), nx_dump_timestamp());
                                #endif

                                if (errno == EINTR)
                                {
                                        continue;
                                }
                        }

                        error("NX> 280 Failed select on input descriptors");

                        goto nx_run_server_side_loop_end;
                }

                while (selected-- > 0)
                {
                        if (FD_ISSET(channel_in, &set))
                        {
                                #ifdef DEBUG
                                logit("NX> 280 Channel in descriptor is selected");
                                #endif

                                in_fd  = channel_in;
                                out_fd = proxy_out;
                        }
                        else
                        {
                                #ifdef DEBUG
                                logit("NX> 280 Proxy in descriptor is selected");
                                #endif

                                in_fd  = proxy_in;
                                out_fd = channel_out;
                        }

                        FD_CLR(in_fd, &set);

                        for (;;)
                        {
                                #ifdef DEBUG
                                logit("NX> 280 Going to read from descriptor: %d at: %s",
                                          in_fd, nx_dump_timestamp());
                                #endif

                                length = read(in_fd, data, sizeof(data));

                                #ifdef DEBUG
                                logit("NX> 280 Read: %d bytes from descriptor: %d at: %s",
                                          length, in_fd, nx_dump_timestamp());
                                #endif

                                if (length <= 0)
                                {
                                        if (length < 0)
                                        {
                                                #ifdef DEBUG
                                                logit("NX> 280 Got error: %d in read: '%s' at: %s",
                                                          errno, strerror(errno), nx_dump_timestamp());
                                                #endif

                                                if (errno == EINTR)
                                                {
                                                        continue;
                                                }
                                        }

                                        #ifdef DEBUG
                                        logit("NX> 280 Error reading from descriptor: %d at: %s",
                                                  in_fd, nx_dump_timestamp());
                                        #endif

                                        failed_fd = in_fd;

                                        goto nx_run_server_side_loop_error;
                                }

                                written = 0;

                                for (;;)
                                {
                                        result = write(out_fd, data + written, length - written);

                                        if (result <= 0)
                                        {
                                                if (result < 0)
                                                {
                                                        #ifdef DEBUG
                                                        logit("NX> 280 Got error: %d in write: '%s' at: %s",
                                                                  errno, strerror(errno), nx_dump_timestamp());
                                                        #endif

                                                        if (errno == EINTR || errno == EAGAIN)
                                                        {
                                                                continue;
                                                        }
                                                }

                                                #ifdef DEBUG
                                                logit("NX> 280 Error writing to descriptor: %d at: %s",
                                                          out_fd, nx_dump_timestamp());
                                                #endif

                                                failed_fd = out_fd;

                                                goto nx_run_server_side_loop_error;
                                        }

                                        #ifdef DEBUG
                                        logit("NX> 280 Written: %d bytes to descriptor: %d at: %s",
                                                  result, out_fd, nx_dump_timestamp());
                                        #endif

                                        written += result;

                                        if (written == length)
                                        {
                                                break;
                                        }
                                }

                                break;
                        }
                }
        }

nx_run_server_side_loop_end:

        error("NX> 280 Exiting from the server side loop");

        return;

nx_run_server_side_loop_error:

        #ifdef NX_SELECTIVELY_CLOSE_DESCRIPTORS

        if (failed_fd == proxy_in)
        {
                error("NX> 290 Failed read on proxy descriptor");

                close(proxy_in);
                close(channel_out);

                proxy_in_open = 0;
        }
        else if (failed_fd == proxy_out)
        {
                error("NX> 290 Failed write on proxy descriptor");

                close(proxy_out);
                close(channel_in);

                channel_in_open = 0;
        }
        else if (failed_fd == channel_in)
        {
                error("NX> 290 Failed read on channel descriptor");

                close(proxy_out);
                close(channel_in);

                channel_in_open = 0;
        }
        else
        {
                error("NX> 290 Failed write on channel descriptor");

                close(proxy_in);
                close(channel_out);

                proxy_in_open = 0;
        }

        #else /* #ifdef NX_SELECTIVELY_CLOSE_DESCRIPTORS */

        if (failed_fd == proxy_in)
        {
                error("NX> 290 Failed read on proxy descriptor");
        }
        else if (failed_fd == proxy_out)
        {
                error("NX> 290 Failed write on proxy descriptor");
        }
        else if (failed_fd == channel_in)
        {
                error("NX> 290 Failed read on channel descriptor");
        }
        else
        {
                error("NX> 290 Failed write on channel descriptor");
        }

        close(proxy_in);
        close(proxy_out);

        close(channel_out);
        close(channel_in);

        proxy_in_open = 0;
        channel_in_open = 0;

        #endif /* #ifdef NX_SELECTIVELY_CLOSE_DESCRIPTORS */

        signal(SIGPIPE, nx_catch_pipe_signal);

        goto nx_run_server_side_loop_start;
}

void nx_run_client_side_loop(int proxy_fd)
{
        /*
         * Close our end of the proxy. The NX library
         * will also close its end and will create a
         * new unencrypted connection to the remote.
         */

        struct timeval timeout;

        close(proxy_fd);

        debug("NX> 285 Entering the client side NX loop");

        while (NXTransRunning(NX_FD_ANY))
        {
                #ifdef DEBUG
                debug("NX> 280 Going to prepare a new NX loop");
                #endif

                timeout.tv_sec = 10;
                timeout.tv_usec = 0;

                /*
                 * Let the proxy run a new loop.
                 */

                NXTransContinue(&timeout);

                #ifdef DEBUG
                debug("NX> 280 Completed execution of the NX loop");
                #endif
        }

        debug("NX> 285 Exiting from the client side loop");

        return;
}

void nx_catch_timeout_signal(int number)
{
}

void nx_catch_pipe_signal(int number)
{
}

void nx_dump_buffer(Buffer *buffer)
{
        unsigned int i;
        unsigned int l;

        unsigned char *p = buffer->d;

        char line[136];

        debug("---");

        for (i = buffer->off, l = 0; i < buffer->size; i++, l++)
        {
                line[l] = p[i];

                if (line[l] == '%')
                {
                        line[l] = '?';
                }

                if (l == 134)
                {
                        line[l]     = '\\';
                        line[l + 1] = '\0';

                        debug("%s", line);

                        l = 0;
                }
        }

        line[l] = '\0';

        if (line[0] != '\0')
        {
            debug("%s", line);
        }

        debug("---");
}

void nx_dump_string(char *string)
{
        int l;

        char *p;

        char line[136];

        debug("---");

        for (p = string, l = 0; *p != '\0'; p++, l++)
        {
                line[l] = *p;

                if (line[l] == '%')
                {
                        line[l] = '?';
                }

                if (l == 134)
                {
                        line[l]     = '\\';
                        line[l + 1] = '\0';

                        debug("%s", line);

                        l = 0;
                }
        }

        line[l] = '\0';

        if (line[0] != '\0')
        {
            debug("%s", line);
        }

        debug("---");
}

#if defined(DEBUG) || defined(TIME)

struct timeval nx_get_timestamp()
{
  struct timeval ts;

  gettimeofday(&ts, NULL);

  return ts;
}

int nx_diff_timestamp(struct timeval ts1, struct timeval ts2)
{
  long ms;

  if (ts1.tv_sec == 0 && ts1.tv_usec == 0)
  {
     return -1;
  }

  /*
   * Add 500 microseconds to round up
   * to the nearest millisecond.
   */

  ms = ((ts2.tv_sec * 1000 + (ts2.tv_usec + 500) / 1000) -
            (ts1.tv_sec * 1000 + (ts1.tv_usec + 500) / 1000));

  return (ms < 0 ? -1 : ms);
}


char *nx_dump_timestamp()
{
  char ctime_new[25];

  char *ctime_now;

  struct timeval ts = nx_get_timestamp();

  ctime_now = ctime((time_t *) &ts.tv_sec);

  sprintf(ctime_new, "%.8s:%3.3f", ctime_now + 11,
              (float) ts.tv_usec / 1000);

  strncpy(ctime_now, ctime_new, 24);

  return ctime_now;
}

#endif

char *nx_search_string_in_buffer(Buffer *buffer, char *start, const char *string)
{
        char *end;
        char *found;

        int temporary;

        if (start == NULL)
        {
                start = buffer_ptr(buffer);
        }

        /*
         * Ensure buffer is either null-terminated
         * or terminated by a newline.
         */

        end = nx_search_newline_in_buffer(buffer, start);

        if (end == NULL)
        {
                return NULL;
        }

        temporary = *end;

        *end = '\0';

        found = strstr(start, string);

        *end = temporary;

        return found;
}

char *nx_remove_string_in_buffer(Buffer *buffer, char *start, int length)
{
        int left;
        int right;

        char *temporary = xmalloc(buffer_len(buffer));

        if (temporary == NULL)
        {
                fatal("NX> 298 Out of memory creating the temporary string");

                return NULL;
        }

        left = start - (char *) buffer_ptr(buffer);

        debug("NX> 280 Lefthand remaining bytes are: %d", left);

        memcpy(temporary, buffer_ptr(buffer), left);

        debug("NX> 280 Righthand remaining bytes are: %d", left);

        right = buffer_len(buffer) - left - length - 1;

        memcpy(temporary + left, start + length + 1, right);

        left += right;

        debug("NX> 280 Total remaining bytes are: %d", left);

        buffer_clear(buffer);

        buffer_append(buffer, temporary, left);

        xfree(temporary);

        return buffer_ptr(buffer);
}

char *nx_search_char_in_buffer(Buffer *buffer, char *start, int value)
{
        char *end;
        char *found;

        int temporary;

        if (start == NULL)
        {
                start = buffer_ptr(buffer);
        }

        /*
         * Ensure buffer is either null-terminated
         * or terminated by a newline.
         */

        end = nx_search_newline_in_buffer(buffer, start);

        if (end == NULL)
        {
                return NULL;
        }

        temporary = *end;

        *end = '\0';

        found = strchr(start, value);

        *end = temporary;

        return found;
}

char *nx_search_newline_in_buffer(Buffer *buffer, char *start)
{
        char *p;
        char *end;

        if (start == NULL)
        {
                start = buffer_ptr(buffer);
        }

        end = (char *) buffer_ptr(buffer) + buffer_len(buffer);

        for (p = start; p < end; p++)
        {
                if (*p == '\n' || *p == '\r' || *p == '\0')
                {
                        return p;
                }
        }

        return NULL;
}

char *nx_remove_newline_in_string(char *start, int length)
{
        char *p;

        char *end = start + length;

        for (p = start; p < end; p++)
        {
                if (*p == '\n' || *p == '\r')
                {
                        *p = '\0';
                }
        }

        return start;
}

char *nx_toupper_string(char *start)
{
        char *p;

        for (p = start; *p != '\0'; p++)
        {
                *p = toupper(*p);
        }

        return start;
}

void nx_wait_timeout(int seconds)
{
        struct timeval timeout;

        timeout.tv_sec  = seconds;
        timeout.tv_usec = 0;

        select(0, NULL, NULL, NULL, &timeout);

        return;
}

void nx_set_socket_options(int fd, int blocking)
{
        /*
         * This is unused at the moment but declared
         * static, so avoid the compiler warning.
         */

        if (0)
        {
                nx_set_keepalive(fd);
        }

        if (blocking == 1)
        {
                nx_set_blocking(fd);
        }
        else
        {
                nx_set_nonblocking(fd);
        }

        nx_set_nodelay(fd);
        nx_set_lowdelay(fd);
}

static int nx_set_nonblocking(int fd)
{
  int flags;

  debug("NX> 286 Setting O_NONBLOCK on descriptor: %d", fd);

  flags = fcntl(fd, F_GETFL);

  if (flags >= 0)
  {
          flags |= O_NONBLOCK;
  }

  if (flags < 0 || fcntl(fd, F_SETFL, flags) < 0)
  {
          error("NX> 286 Failed to set O_NONBLOCK on descriptor: %d", fd);

          return -1;
  }

  debug("NX> 286 Set O_NONBLOCK on descriptor: %d", fd);

  return 1;
}

static int nx_set_blocking(int fd)
{
  int flags;

  debug("NX> 286 Resetting O_NONBLOCK on descriptor: %d", fd);

  flags = fcntl(fd, F_GETFL);

  if (flags >= 0)
  {
          flags &= ~O_NONBLOCK;
  }

  if (flags < 0 || fcntl(fd, F_SETFL, flags) < 0)
  {
          error("NX> 286 Failed to reset O_NONBLOCK on descriptor: %d", fd);

          return -1;
  }

  debug("NX> 286 Reset O_NONBLOCK on descriptor: %d", fd);

  return 1;
}

static int nx_set_nodelay(int fd)
{
        int result;

        int flag = 1;

        debug("NX> 286 Trying TCP_NODELAY on descriptor: %d", fd);

        result = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        if (result == 0)
        {
          result = 1;
        }
        else if (result < 0)
        {
                #if defined(__APPLE__) || defined(__sun)

                if (errno == ENOPROTOOPT)
                {
                        result = 0;
                }

                #else

                if (errno == EOPNOTSUPP)
                {
                        result = 0;
                }

                #endif
        }

        if (result < 0)
        {
                error("NX> 286 Failed to set TCP_NODELAY on descriptor: %d", fd);
        }
        else if (result == 0)
        {
                debug("NX> 286 Option TCP_NODELAY not supported on: %d", fd);
        }
        else
        {
                debug("NX> 286 Set TCP_NODELAY on descriptor: %d", fd);
        }

        return result;
}

static int nx_set_keepalive(int fd)
{
        int result;

        int flag = 1;

        debug("NX> 286 Trying SO_KEEPALIVE on descriptor: %d", fd);

        result = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));

        if (result < 0)
        {
                error("NX> 286 Failed to set SO_KEEPALIVE on descriptor: %d", fd);
        }
        else
        {
                debug("NX> 286 Set SO_KEEPALIVE on descriptor: %d", fd);
        }

        return result;
}

static int nx_set_lowdelay(int fd)
{
        int result;

        int flag = IPTOS_LOWDELAY;

        #if defined(__CYGWIN32__)

        return 0;

        #endif

        debug("NX> 286 Trying IPTOS_LOWDELAY on descriptor: %d", fd);

        result = setsockopt(fd, IPPROTO_IP, IP_TOS, &flag, sizeof(flag));

        if (result == 0)
        {
          result = 1;
        }
        else if (result < 0)
        {
                #if defined(__APPLE__) || defined(__sun)

                if (errno == ENOPROTOOPT)
                {
                        result = 0;
                }

                #else

                if (errno == EOPNOTSUPP)
                {
                        result = 0;
                }

                #endif
        }

        if (result < 0)
        {
                error("NX> 286 Failed to set IPTOS_LOWDELAY on descriptor: %d", fd);
        }
        else if (result == 0)
        {
                debug("NX> 286 Option IPTOS_LOWDELAY not supported on: %d", fd);
        }
        else
        {
                debug("NX> 286 Set IPTOS_LOWDELAY on descriptor: %d", fd);
        }

        return result;
}

void nx_redirect_log_output()
{
        FILE *file;

        char name[1024];

        const char *proxy_log;

        logit("NX> 285 Enabling redirection of debug output");

        /*
         * Try to get the name of the file from
         * the NX transport.
         */

        proxy_log = NXTransFile(NX_FILE_ERRORS);

        if (proxy_log != NULL)
        {
                logit("NX> 280 Using proxy log file: %s", proxy_log);

                strncpy(name, proxy_log, 1023);

                *(name + 1023) = '\0';
        }
        else
        {
                #ifdef NX_SHARE_BINDER_LOG

                /*
                 * Use a well-known file so that you can force
                 * the proxy to open the same file. This file
                 * is also made writable by everybody. The for-
                 * warding process, in fact, is usually run as
                 * the nx user while the proxy runs as the real
                 * account.
                 */

                strcpy(name, "/tmp/errors");

                logit("NX> 280 Using log file: %s", name);

                #else

                logit("NX> 280 Can't determine the name of the proxy log");

                return;

                #endif
        }

        file = fopen(name, "a");

        if (file == NULL)
        {
                logit("NX> 280 Can't open log file: %s error is: %d, '%s'",
                          name, errno, strerror(errno));
        }
        else
        {
                if (dup2(fileno(file), fileno(stderr)) == -1)
                {
                        logit("NX> 280 Can't redirect the log to file: %s", name);
                }
        }
}

const char *nx_get_environment(const char *name)
{
        return getenv(name);
}

int nx_set_environment(const char *name, const char *value)
{
        #ifdef __sun

        static char buffer[1024];

        snprintf(buffer, 1024 - 1, "%s=%s", name, value);

        *(buffer + 1023) = '\0';

        return putenv(buffer);

        #else

        return setenv(name, value, 1);

        #endif
}
