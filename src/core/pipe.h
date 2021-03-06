//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef CORE_PIPE_H
#define CORE_PIPE_H

// NB: This structure is supplied here for use by the CORE. Use of this
// OUSIDE of the core is STRICTLY VERBOTEN.  NO DIRECT ACCESS BY PROTOCOLS OR
// TRANSPORTS.

#include "core/defs.h"
#include "core/transport.h"

struct nng_pipe {
	uint32_t	p_id;
	nni_tran_pipe	p_tran_ops;
	void *		p_tran_data;
	void *		p_proto_data;
	nni_list_node	p_node;
	nni_sock *	p_sock;
	nni_ep *	p_ep;
	int		p_reap;
	int		p_active;
	nni_thr		p_worker_thr[NNI_MAXWORKERS];
};

// Pipe operations that protocols use.
extern int nni_pipe_recv(nni_pipe *, nng_msg **);
extern int nni_pipe_send(nni_pipe *, nng_msg *);
extern uint32_t nni_pipe_id(nni_pipe *);
extern void nni_pipe_close(nni_pipe *);

// Used only by the socket core - as we don't wish to expose the details
// of the pipe structure outside of pipe.c.
extern int nni_pipe_create(nni_pipe **, nni_ep *);

extern void nni_pipe_destroy(nni_pipe *);

extern uint16_t nni_pipe_proto(nni_pipe *);
extern uint16_t nni_pipe_peer(nni_pipe *);
extern int nni_pipe_start(nni_pipe *);
extern int nni_pipe_getopt(nni_pipe *, int, void *, size_t *sizep);

#endif // CORE_PIPE_H
