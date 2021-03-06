/*
 * General mechanism for wrapping up reading/writing of Windows
 * HANDLEs into a PuTTY Socket abstraction.
 */

#include <stdio.h>
#include <assert.h>
#include <limits.h>

#define DEFINE_PLUG_METHOD_MACROS
#include "tree234.h"
#include "putty.h"
#include "network.h"

typedef struct HandleSocket {
    HANDLE send_H, recv_H, stderr_H;
    struct handle *send_h, *recv_h, *stderr_h;

    /*
     * Freezing one of these sockets is a slightly fiddly business,
     * because the reads from the handle are happening in a separate
     * thread as blocking system calls and so once one is in progress
     * it can't sensibly be interrupted. Hence, after the user tries
     * to freeze one of these sockets, it's unavoidable that we may
     * receive one more load of data before we manage to get
     * winhandl.c to stop reading.
     */
    enum {
        UNFROZEN,  /* reading as normal */
        FREEZING,  /* have been set to frozen but winhandl is still reading */
        FROZEN,    /* really frozen - winhandl has been throttled */
        THAWING    /* we're gradually releasing our remaining data */
    } frozen;
    /* We buffer data here if we receive it from winhandl while frozen. */
    bufchain inputdata;

    /* Data received from stderr_H, if we have one. */
    bufchain stderrdata;

    int defer_close, deferred_close;   /* in case of re-entrance */

    char *error;

    Plug plug;

    const Socket_vtable *sockvt;
} HandleSocket;

static int handle_gotdata(struct handle *h, void *data, int len)
{
    HandleSocket *hs = (HandleSocket *)handle_get_privdata(h);

    if (len < 0) {
	plug_closing(hs->plug, "Read error from handle", 0, 0);
	return 0;
    } else if (len == 0) {
	plug_closing(hs->plug, NULL, 0, 0);
	return 0;
    } else {
        assert(hs->frozen != FROZEN && hs->frozen != THAWING);
        if (hs->frozen == FREEZING) {
            /*
             * If we've received data while this socket is supposed to
             * be frozen (because the read winhandl.c started before
             * sk_set_frozen was called has now returned) then buffer
             * the data for when we unfreeze.
             */
            bufchain_add(&hs->inputdata, data, len);
            hs->frozen = FROZEN;

            /*
             * And return a very large backlog, to prevent further
             * data arriving from winhandl until we unfreeze.
             */
            return INT_MAX;
        } else {
            plug_receive(hs->plug, 0, data, len);
	    return 0;
        }
    }
}

static int handle_stderr(struct handle *h, void *data, int len)
{
    HandleSocket *hs = (HandleSocket *)handle_get_privdata(h);

    if (len > 0)
        log_proxy_stderr(hs->plug, &hs->stderrdata, data, len);

    return 0;
}

static void handle_sentdata(struct handle *h, int new_backlog)
{
    HandleSocket *hs = (HandleSocket *)handle_get_privdata(h);

    if (new_backlog < 0) {
        /* Special case: this is actually reporting an error writing
         * to the underlying handle, and our input value is the error
         * code itself, negated. */
        plug_closing(hs->plug, win_strerror(-new_backlog), -new_backlog, 0);
        return;
    }

    plug_sent(hs->plug, new_backlog);
}

static Plug sk_handle_plug(Socket s, Plug p)
{
    HandleSocket *hs = FROMFIELD(s, HandleSocket, sockvt);
    Plug ret = hs->plug;
    if (p)
	hs->plug = p;
    return ret;
}

static void sk_handle_close(Socket s)
{
    HandleSocket *hs = FROMFIELD(s, HandleSocket, sockvt);

    if (hs->defer_close) {
        hs->deferred_close = TRUE;
        return;
    }

    handle_free(hs->send_h);
    handle_free(hs->recv_h);
    CloseHandle(hs->send_H);
    if (hs->recv_H != hs->send_H)
        CloseHandle(hs->recv_H);
    bufchain_clear(&hs->inputdata);
    bufchain_clear(&hs->stderrdata);

    sfree(hs);
}

static int sk_handle_write(Socket s, const void *data, int len)
{
    HandleSocket *hs = FROMFIELD(s, HandleSocket, sockvt);

    return handle_write(hs->send_h, data, len);
}

static int sk_handle_write_oob(Socket s, const void *data, int len)
{
    /*
     * oob data is treated as inband; nasty, but nothing really
     * better we can do
     */
    return sk_handle_write(s, data, len);
}

static void sk_handle_write_eof(Socket s)
{
    HandleSocket *hs = FROMFIELD(s, HandleSocket, sockvt);

    handle_write_eof(hs->send_h);
}

static void sk_handle_flush(Socket s)
{
    /* HandleSocket *hs = FROMFIELD(s, HandleSocket, sockvt); */
    /* do nothing */
}

static void handle_socket_unfreeze(void *hsv)
{
    HandleSocket *hs = (HandleSocket *)hsv;
    void *data;
    int len;

    /*
     * If we've been put into a state other than THAWING since the
     * last callback, then we're done.
     */
    if (hs->frozen != THAWING)
        return;

    /*
     * Get some of the data we've buffered.
     */
    bufchain_prefix(&hs->inputdata, &data, &len);
    assert(len > 0);

    /*
     * Hand it off to the plug. Be careful of re-entrance - that might
     * have the effect of trying to close this socket.
     */
    hs->defer_close = TRUE;
    plug_receive(hs->plug, 0, data, len);
    bufchain_consume(&hs->inputdata, len);
    hs->defer_close = FALSE;
    if (hs->deferred_close) {
        sk_handle_close(&hs->sockvt);
        return;
    }

    if (bufchain_size(&hs->inputdata) > 0) {
        /*
         * If there's still data in our buffer, stay in THAWING state,
         * and reschedule ourself.
         */
        queue_toplevel_callback(handle_socket_unfreeze, hs);
    } else {
        /*
         * Otherwise, we've successfully thawed!
         */
        hs->frozen = UNFROZEN;
        handle_unthrottle(hs->recv_h, 0);
    }
}

static void sk_handle_set_frozen(Socket s, int is_frozen)
{
    HandleSocket *hs = FROMFIELD(s, HandleSocket, sockvt);

    if (is_frozen) {
        switch (hs->frozen) {
          case FREEZING:
          case FROZEN:
            return;                    /* nothing to do */

          case THAWING:
            /*
             * We were in the middle of emptying our bufchain, and got
             * frozen again. In that case, winhandl.c is already
             * throttled, so just return to FROZEN state. The toplevel
             * callback will notice and disable itself.
             */
            hs->frozen = FROZEN;
            break;

          case UNFROZEN:
            /*
             * The normal case. Go to FREEZING, and expect one more
             * load of data from winhandl if we're unlucky.
             */
            hs->frozen = FREEZING;
            break;
        }
    } else {
        switch (hs->frozen) {
          case UNFROZEN:
          case THAWING:
            return;                    /* nothing to do */

          case FREEZING:
            /*
             * If winhandl didn't send us any data throughout the time
             * we were frozen, then we'll still be in this state and
             * can just unfreeze in the trivial way.
             */
            assert(bufchain_size(&hs->inputdata) == 0);
            hs->frozen = UNFROZEN;
            break;

          case FROZEN:
            /*
             * If we have buffered data, go to THAWING and start
             * releasing it in top-level callbacks.
             */
            hs->frozen = THAWING;
            queue_toplevel_callback(handle_socket_unfreeze, hs);
        }
    }
}

static const char *sk_handle_socket_error(Socket s)
{
    HandleSocket *hs = FROMFIELD(s, HandleSocket, sockvt);
    return hs->error;
}

static char *sk_handle_peer_info(Socket s)
{
    HandleSocket *hs = FROMFIELD(s, HandleSocket, sockvt);
    ULONG pid;
    static HMODULE kernel32_module;
    DECL_WINDOWS_FUNCTION(static, BOOL, GetNamedPipeClientProcessId,
                          (HANDLE, PULONG));

    if (!kernel32_module) {
        kernel32_module = load_system32_dll("kernel32.dll");
#if (defined _MSC_VER && _MSC_VER < 1900) || defined __MINGW32__ || defined COVERITY
        /* For older Visual Studio, and MinGW too (at least as of
         * Ubuntu 16.04), this function isn't available in the header
         * files to type-check. Ditto the toolchain I use for
         * Coveritying the Windows code. */
        GET_WINDOWS_FUNCTION_NO_TYPECHECK(
            kernel32_module, GetNamedPipeClientProcessId);
#else
        GET_WINDOWS_FUNCTION(
            kernel32_module, GetNamedPipeClientProcessId);
#endif
    }

    /*
     * Of course, not all handles managed by this module will be
     * server ends of named pipes, but if they are, then it's useful
     * to log what we can find out about the client end.
     */
    if (p_GetNamedPipeClientProcessId &&
        p_GetNamedPipeClientProcessId(hs->send_H, &pid))
        return dupprintf("process id %lu", (unsigned long)pid);

    return NULL;
}

static const Socket_vtable HandleSocket_sockvt = {
    sk_handle_plug,
    sk_handle_close,
    sk_handle_write,
    sk_handle_write_oob,
    sk_handle_write_eof,
    sk_handle_flush,
    sk_handle_set_frozen,
    sk_handle_socket_error,
    sk_handle_peer_info,
};

Socket make_handle_socket(HANDLE send_H, HANDLE recv_H, HANDLE stderr_H,
                          Plug plug, int overlapped)
{
    HandleSocket *hs;
    int flags = (overlapped ? HANDLE_FLAG_OVERLAPPED : 0);

    hs = snew(HandleSocket);
    hs->sockvt = &HandleSocket_sockvt;
    hs->plug = plug;
    hs->error = NULL;
    hs->frozen = UNFROZEN;
    bufchain_init(&hs->inputdata);
    bufchain_init(&hs->stderrdata);

    hs->recv_H = recv_H;
    hs->recv_h = handle_input_new(hs->recv_H, handle_gotdata, hs, flags);
    hs->send_H = send_H;
    hs->send_h = handle_output_new(hs->send_H, handle_sentdata, hs, flags);
    hs->stderr_H = stderr_H;
    if (hs->stderr_H)
        hs->stderr_h = handle_input_new(hs->stderr_H, handle_stderr,
                                        hs, flags);

    hs->defer_close = hs->deferred_close = FALSE;

    return &hs->sockvt;
}
