//
// Copyright 2016 Garrett D'Amore <garrett@damore.org>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef CORE_SOCKET_H
#define CORE_SOCKET_H

// NB: This structure is supplied here for use by the CORE. Use of this library
// OUSIDE of the core is STRICTLY VERBOTEN.  NO DIRECT ACCESS BY PROTOCOLS OR
// TRANSPORTS.
struct nng_socket {
	nni_mtx		s_mx;
	nni_cv		s_cv;

	nni_msgq *	s_uwq;          // Upper write queue
	nni_msgq *	s_urq;          // Upper read queue

	nni_protocol	s_ops;

	void *		s_data;         // Protocol private

	// XXX: options
	nni_duration	s_linger;               // linger time
	nni_duration	s_sndtimeo;             // send timeout
	nni_duration	s_rcvtimeo;             // receive timeout
	nni_duration	s_reconn;               // reconnect time
	nni_duration	s_reconnmax;            // max reconnect time

	nni_list	s_eps;                  // active endpoints
	nni_list	s_pipes;                // pipes for this socket

	nni_list	s_reaps;                // pipes to reap
	nni_thr		s_reaper;

	int		s_closing;              // Socket is closing
	int		s_besteffort;           // Best effort mode delivery
	int		s_senderr;              // Protocol state machine use
	int		s_recverr;              // Protocol state machine use

	uint32_t	s_nextid;               // Next Pipe ID.
};

extern int nni_socket_create(nni_socket **, uint16_t);
extern int nni_socket_close(nni_socket *);
extern uint16_t nni_socket_proto(nni_socket *);
extern int nni_socket_setopt(nni_socket *, int, const void *, size_t);
extern int nni_socket_getopt(nni_socket *, int, void *, size_t *);
extern int nni_socket_recvmsg(nni_socket *, nni_msg **, nni_time);
extern int nni_socket_sendmsg(nni_socket *, nni_msg *, nni_time);
extern int nni_socket_dial(nni_socket *, const char *, nni_endpt **, int);
extern int nni_socket_listen(nni_socket *, const char *, nni_endpt **, int);

// Set error codes for applications.  These are only ever
// called from the filter functions in protocols, and thus
// already have the socket lock held.
extern void nni_socket_recverr(nni_socket *, int);
extern void nni_socket_senderr(nni_socket *, int);

#endif  // CORE_SOCKET_H
