//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"

// Request protocol.  The REQ protocol is the "request" side of a
// request-reply pair.  This is useful for building RPC clients, for
// example.

typedef struct nni_req_pipe	nni_req_pipe;
typedef struct nni_req_sock	nni_req_sock;

// An nni_req_sock is our per-socket protocol private structure.
struct nni_req_sock {
	nni_sock *	sock;
	nni_cv		cv;
	nni_msgq *	uwq;
	nni_msgq *	urq;
	nni_duration	retry;
	nni_time	resend;
	int		raw;
	int		closing;
	nni_msg *	reqmsg;
	nni_msg *	retrymsg;
	uint32_t	nextid;         // next id
	uint8_t		reqid[4];       // outstanding request ID (big endian)
};

// An nni_req_pipe is our per-pipe protocol private structure.
struct nni_req_pipe {
	nni_pipe *	pipe;
	nni_req_sock *	req;
	int		sigclose;
};

static void nni_req_resender(void *);

static int
nni_req_sock_init(void **reqp, nni_sock *sock)
{
	nni_req_sock *req;
	int rv;

	if ((req = NNI_ALLOC_STRUCT(req)) == NULL) {
		return (NNG_ENOMEM);
	}
	if ((rv = nni_cv_init(&req->cv, nni_sock_mtx(sock))) != 0) {
		NNI_FREE_STRUCT(req);
		return (rv);
	}
	// this is "semi random" start for request IDs.
	req->nextid = nni_random();
	req->retry = NNI_SECOND * 60;
	req->sock = sock;
	req->reqmsg = NULL;
	req->retrymsg = NULL;
	req->raw = 0;
	req->resend = NNI_TIME_ZERO;

	req->uwq = nni_sock_sendq(sock);
	req->urq = nni_sock_recvq(sock);
	*reqp = req;
	nni_sock_recverr(sock, NNG_ESTATE);
	return (0);
}


static void
nni_req_sock_close(void *arg)
{
	nni_req_sock *req = arg;

	req->closing = 1;
	nni_cv_wake(&req->cv);
}


static void
nni_req_sock_fini(void *arg)
{
	nni_req_sock *req = arg;

	nni_cv_fini(&req->cv);
	if (req->reqmsg != NULL) {
		nni_msg_free(req->reqmsg);
	}
	if (req->retrymsg != NULL) {
		nni_msg_free(req->retrymsg);
	}
	NNI_FREE_STRUCT(req);
}


static int
nni_req_pipe_init(void **rpp, nni_pipe *pipe, void *rsock)
{
	nni_req_pipe *rp;

	if ((rp = NNI_ALLOC_STRUCT(rp)) == NULL) {
		return (NNG_ENOMEM);
	}
	rp->pipe = pipe;
	rp->sigclose = 0;
	rp->req = rsock;
	*rpp = rp;
	return (0);
}


static void
nni_req_pipe_fini(void *arg)
{
	nni_req_pipe *rp = arg;

	NNI_FREE_STRUCT(rp);
}


static int
nni_req_pipe_add(void *arg)
{
	nni_req_pipe *rp = arg;

	if (nni_pipe_peer(rp->pipe) != NNG_PROTO_REP) {
		return (NNG_EPROTO);
	}
	return (0);
}


static void
nni_req_pipe_rem(void *arg)
{
	// As with add, nothing to do here.
	NNI_ARG_UNUSED(arg);
}


static void
nni_req_pipe_send(void *arg)
{
	nni_req_pipe *rp = arg;
	nni_req_sock *req = rp->req;
	nni_msgq *uwq = req->uwq;
	nni_msgq *urq = req->urq;
	nni_pipe *pipe = rp->pipe;
	nni_mtx *mx = nni_sock_mtx(req->sock);
	nni_msg *msg;
	int rv;

	for (;;) {
		nni_mtx_lock(mx);
		if ((msg = req->retrymsg) != NULL) {
			req->retrymsg = NULL;
		}
		nni_mtx_unlock(mx);
		if (msg == NULL) {
			rv = nni_msgq_get_sig(uwq, &msg, &rp->sigclose);
			if (rv != 0) {
				break;
			}
		}
		rv = nni_pipe_send(pipe, msg);
		if (rv != 0) {
			nni_msg_free(msg);
			break;
		}
	}
	nni_msgq_signal(urq, &rp->sigclose);
	nni_pipe_close(pipe);
}


static void
nni_req_pipe_recv(void *arg)
{
	nni_req_pipe *rp = arg;
	nni_req_sock *req = rp->req;
	nni_msgq *urq = req->urq;
	nni_msgq *uwq = req->uwq;
	nni_pipe *pipe = rp->pipe;
	nni_msg *msg;
	int rv;

	for (;;) {
		rv = nni_pipe_recv(pipe, &msg);
		if (rv != 0) {
			break;
		}
		// We yank 4 bytes of body, and move them to the header.
		if (nni_msg_len(msg) < 4) {
			// Not enough data, just toss it.
			nni_msg_free(msg);
			continue;
		}
		if (nni_msg_append_header(msg, nni_msg_body(msg), 4) != 0) {
			// Should be NNG_ENOMEM
			nni_msg_free(msg);
			continue;
		}
		if (nni_msg_trim(msg, 4) != 0) {
			// This should never happen - could be an assert.
			nni_panic("Failed to trim REQ header from body");
		}
		rv = nni_msgq_put_sig(urq, msg, &rp->sigclose);
		if (rv != 0) {
			nni_msg_free(msg);
			break;
		}
	}
	nni_msgq_signal(uwq, &rp->sigclose);
	nni_pipe_close(pipe);
}


static int
nni_req_sock_setopt(void *arg, int opt, const void *buf, size_t sz)
{
	nni_req_sock *req = arg;
	int rv;

	switch (opt) {
	case NNG_OPT_RESENDTIME:
		rv = nni_setopt_duration(&req->retry, buf, sz);
		break;
	case NNG_OPT_RAW:
		rv = nni_setopt_int(&req->raw, buf, sz, 0, 1);
		break;
	default:
		rv = NNG_ENOTSUP;
	}
	return (rv);
}


static int
nni_req_sock_getopt(void *arg, int opt, void *buf, size_t *szp)
{
	nni_req_sock *req = arg;
	int rv;

	switch (opt) {
	case NNG_OPT_RESENDTIME:
		rv = nni_getopt_duration(&req->retry, buf, szp);
		break;
	case NNG_OPT_RAW:
		rv = nni_getopt_int(&req->raw, buf, szp);
		break;
	default:
		rv = NNG_ENOTSUP;
	}
	return (rv);
}


static void
nni_req_sock_resend(void *arg)
{
	nni_req_sock *req = arg;
	nni_mtx *mx = nni_sock_mtx(req->sock);
	int rv;

	for (;;) {
		nni_mtx_lock(mx);
		if (req->closing) {
			nni_mtx_unlock(mx);
			return;
		}
		if (req->reqmsg == NULL) {
			nni_cv_wait(&req->cv);
			nni_mtx_unlock(mx);
			continue;
		}
		rv = nni_cv_until(&req->cv, req->resend);
		if ((rv == NNG_ETIMEDOUT) && (req->reqmsg != NULL)) {
			// XXX: check for final timeout on this?
			if (req->retrymsg == NULL) {
				nni_msg_dup(&req->retrymsg, req->reqmsg);
			}
			req->resend = nni_clock() + req->retry;
		}
		nni_mtx_unlock(mx);
	}
}


static nni_msg *
nni_req_sock_sfilter(void *arg, nni_msg *msg)
{
	nni_req_sock *req = arg;
	uint32_t id;

	if (req->raw) {
		// No automatic retry, and the request ID must
		// be in the header coming down.
		return (msg);
	}

	// Generate a new request ID.  We always set the high
	// order bit so that the peer can locate the end of the
	// backtrace.  (Pipe IDs have the high order bit clear.)
	id = (req->nextid++) | 0x80000000u;

	// Request ID is in big endian format.
	NNI_PUT32(req->reqid, id);

	if (nni_msg_append_header(msg, req->reqid, 4) != 0) {
		// Should be ENOMEM.
		nni_msg_free(msg);
		return (NULL);
	}

	// If another message is there, this cancels it.
	if (req->reqmsg != NULL) {
		nni_msg_free(req->reqmsg);
		req->reqmsg = NULL;
	}

	// Make a duplicate message... for retries.
	if (nni_msg_dup(&req->reqmsg, msg) != 0) {
		nni_msg_free(msg);
		return (NULL);
	}

	// Schedule the next retry
	req->resend = nni_clock() + req->retry;
	nni_cv_wake(&req->cv);

	// Clear the error condition.
	nni_sock_recverr(req->sock, 0);

	return (msg);
}


static nni_msg *
nni_req_sock_rfilter(void *arg, nni_msg *msg)
{
	nni_req_sock *req = arg;

	if (req->raw) {
		// Pass it unmolested
		return (msg);
	}

	if (nni_msg_header_len(msg) < 4) {
		nni_msg_free(msg);
		return (NULL);
	}

	if (req->reqmsg == NULL) {
		// We had no outstanding request.
		nni_msg_free(msg);
		return (NULL);
	}
	if (memcmp(nni_msg_header(msg), req->reqid, 4) != 0) {
		// Wrong request id
		nni_msg_free(msg);
		return (NULL);
	}

	nni_sock_recverr(req->sock, NNG_ESTATE);
	nni_msg_free(req->reqmsg);
	req->reqmsg = NULL;
	nni_cv_wake(&req->cv);
	return (msg);
}


// This is the global protocol structure -- our linkage to the core.
// This should be the only global non-static symbol in this file.
static nni_proto_pipe_ops nni_req_pipe_ops = {
	.pipe_init	= nni_req_pipe_init,
	.pipe_fini	= nni_req_pipe_fini,
	.pipe_add	= nni_req_pipe_add,
	.pipe_rem	= nni_req_pipe_rem,
	.pipe_worker	= { nni_req_pipe_send,
			    nni_req_pipe_recv },
};

static nni_proto_sock_ops nni_req_sock_ops = {
	.sock_init	= nni_req_sock_init,
	.sock_fini	= nni_req_sock_fini,
	.sock_close	= nni_req_sock_close,
	.sock_setopt	= nni_req_sock_setopt,
	.sock_getopt	= nni_req_sock_getopt,
	.sock_rfilter	= nni_req_sock_rfilter,
	.sock_sfilter	= nni_req_sock_sfilter,
	.sock_worker	= { nni_req_sock_resend },
};

nni_proto nni_req_proto = {
	.proto_self	= NNG_PROTO_REQ,
	.proto_peer	= NNG_PROTO_REP,
	.proto_name	= "req",
	.proto_sock_ops = &nni_req_sock_ops,
	.proto_pipe_ops = &nni_req_pipe_ops,
};
