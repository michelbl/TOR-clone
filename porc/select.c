#include "select.h"



int send_to_relay(char *buffer, int n, int porc_session_id) {
	return 0;
}


int set_fds (int *nfds, fd_set *fds) {
	CLIENT_CHAINED_LIST_LINK *c;
	int max = -2;
	int n=1;

	FD_ZERO (fds);
	FD_SET (client_circuit.relay1_socket_descriptor, fds);
	for (c=porc_sessions.first; c!=NULL; c=c->nxt) {
		FD_SET (c->item.client_socket_descriptor, fds);
		n++;
		if (c->item.client_socket_descriptor>max) {
			max = c->item.client_socket_descriptor;
		}
	}

	*nfds = max + 1;
	return n;	
}


/*
	do_proxy - Process existing PORC client connections.

	Do proxy between a clear connections and a secure connection.
*/
int do_proxy() {
	fd_set read_fds;
	int ret, nbr;
	int nfds;
	char buffer[BUF_SIZE+1];
	CLIENT_CHAINED_LIST_LINK *c;
	sigset_t signal_set_tmp, signal_set;

	sigemptyset(&signal_set_tmp);
	ret = pthread_sigmask (SIG_BLOCK, &signal_set_tmp, &signal_set);
	if (ret != 0) {
		fprintf (stderr, "Impossible to get the current signal mask.\n");
		return -1;
	}
	ret = sigdelset (&signal_set, SIGUSR1);
	if (ret != 0) {
		fprintf (stderr, "Impossible to prepare the signal mask.\n");
		return -1;
	}

	for (;;) {
		if (set_fds (&nfds, &read_fds) == -1) {
			fprintf (stderr, "Preventing a dead-lock.\n");
			return -1;
		}

		while((nbr = pselect(nfds, &read_fds, 0, 0, 0, &signal_set)) > 0) {
			if (FD_ISSET (client_circuit.relay1_socket_descriptor, &read_fds)) {
				int recvd = gnutls_record_recv(client_circuit.session, buffer, BUF_SIZE);
				if(recvd <= 0) {
					fprintf (stderr, "Stop (50) on relay reception\n");
					return -1;
				}
				buffer [recvd] = '\0';
				printf ("Receiving from relay (%d bytes) : %s\n", recvd, buffer);
				if (send_to_relay(buffer, recvd, c->item.id)!=0) {
					fprintf (stderr, "Stop (70), %d\n", c->item.id);
					return -1;
				}
			}
			for (c=porc_sessions.first; c!=NULL; c=c->nxt) {
				if (FD_ISSET (c->item.client_socket_descriptor, &read_fds)) {
					int recvd = recv(c->item.client_socket_descriptor, buffer, BUF_SIZE, 0);
					if(recvd <= 0) {
						fprintf (stderr, "Stop (100), %d\n", c->item.id);
						return -1;
					}
					buffer [recvd] = '\0';
					printf ("Receiving from client (%d bytes) : %s\n", recvd, buffer);
					if (send_to_relay(buffer, recvd, c->item.id)!=0) {
						fprintf (stderr, "Stop (250), %d\n", c->item.id);
						return -1;
					}
				}
			}
		}
	}

	return 0;
}

