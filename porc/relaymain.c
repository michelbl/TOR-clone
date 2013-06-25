/*
	relay - PORC relay

	The PORC relay transfers a stream to another relay or to the target.
*/

#include "relaymain.h"

static gnutls_priority_t priority_cache;

pthread_t accepting_thread;
pthread_t selecting_thread;

gcry_sexp_t * publicKey; 
gcry_sexp_t * privateKey;


CHAINED_LIST tls_session_list;
CHAINED_LIST porc_session_list;
CHAINED_LIST socks_session_list;


/*
	handle_connection - Sets up a TLS and a PORC session with a client or relay.
*/
int handle_connection(int client_socket_descriptor) {
	gnutls_session_t session;
	int ret;

	gnutls_init (&session, GNUTLS_SERVER);
	gnutls_priority_set (session, priority_cache);
	gnutls_credentials_set (session, GNUTLS_CRD_CERTIFICATE, xcred);
	gnutls_certificate_server_set_request (session, GNUTLS_CERT_IGNORE);

	gnutls_transport_set_int (session, client_socket_descriptor);
	do {
		ret = gnutls_handshake (session);
	}
	while (ret < 0 && gnutls_error_is_fatal (ret) == 0);

	if (ret < 0) {
		close (client_socket_descriptor);
		gnutls_deinit (session);
		fprintf (stderr, "*** Handshake has failed (%s)\n\n", gnutls_strerror (ret));
		return -1;
	}
	printf ("Tls handshake was completed\n");
	
	// Record TLS session
	ITEM_TLS_SESSION * tls_session;
	int tls_session_id;
	tls_session_id = ChainedListNew (&tls_session_list, (void**) &tls_session, sizeof(ITEM_TLS_SESSION));
	tls_session->socket_descriptor = client_socket_descriptor;
	tls_session->session = session;
	
	
	// Start a PORC session
	// wait for asking public key
	PUB_KEY_REQUEST pub_key_request;
	if (gnutls_record_recv (session, (char *)&pub_key_request, 
		sizeof (pub_key_request)) != sizeof (pub_key_request))
	{
		fprintf (stderr, "Error in public key request (router)\n");
		return -1;
	}
	if (pub_key_request.command != PUB_KEY_ASK)
	{
		fprintf (stderr, "Error : invalid public key request\n");
		return -1;
	}
	//Send public Key
	PUB_KEY_RESPONSE pub_key_response;
	char * export_pub_key = malloc(PUBLIC_KEY_LEN);
	if (rsaExportKey(publicKey, export_pub_key)!=0)
	{
		fprintf (stderr, "Error exporting public key (router)\n");
		return -1;
	}
	pub_key_response.status = PUB_KEY_SUCCESS;
	memcpy(pub_key_response.public_key,export_pub_key,PUBLIC_KEY_LEN);
	if (gnutls_record_send (session, (char *)&pub_key_response, 
		sizeof (pub_key_response)) != sizeof (pub_key_response))
	{
		fprintf (stderr, "Error sending public key (router)\n");
		return -1;
	}
	free(export_pub_key);
	//Wait for the symmetric key
	CRYPT_SYM_KEY_RESPONSE crypt_sym_key_response;
	if (gnutls_record_recv (session, (char *)&crypt_sym_key_response, 
		sizeof (crypt_sym_key_response)) != sizeof (crypt_sym_key_response)) 
	{
		fprintf (stderr, "Error while awaiting symmetric key (router)\n");
		return -1;	
	}
	if (crypt_sym_key_response.status != CRYPT_SYM_KEY_SUCCESS)
	{
		fprintf (stderr, "Error : invalid symmetric key\n");
		return -1;
	}

	// Record the PORC session
	ITEM_PORC_SESSION *porc_session;
	int id_porc_session;

	id_porc_session = ChainedListNew (&porc_session_list, (void *)&porc_session, sizeof(ITEM_PORC_SESSION));
	if (rsaDecrypt(crypt_sym_key_response.crypt_sym_key, porc_session->sym_key, *privateKey)!=0)
	{
		fprintf (stderr, "Error decrypting symmetric key\n");
		return -1;
	}
	//Record into the structure
	porc_session->id_prev = pub_key_request.porc_session; 
	porc_session->client_tls_session = tls_session_id;
	porc_session->final = 1;
	porc_session->server_tls_session = 0;


	// Signaling a new available socket to the selecting thread
	if (pthread_kill (selecting_thread, SIGUSR1) != 0) {
		fprintf (stderr, "Signal sending failed\n");
		return -1;
	}	

	return 0;
}



int accepting (int listen_socket_descriptor) {
	struct sockaddr_in sockaddr_client;
	int client_socket_descriptor;
	socklen_t length = sizeof(sockaddr_client);
	int ret;

	for (;;) {
		if ((client_socket_descriptor = accept(listen_socket_descriptor, (struct sockaddr *) &sockaddr_client, &length)) > 0) {
			printf ("New client %d\n", client_socket_descriptor);
			ret = handle_connection (client_socket_descriptor);
			if (ret != 0) {
				break;
			}
		}
	}

	return ret;
}


void *start_accepting (void *arg) {
	return ((void *)accepting((int)arg));
}
	




int set_fds (int *nfds, fd_set *fds) {
	CHAINED_LIST_LINK *c;
	int max = -2;
	int n=1;

	FD_ZERO (fds);

	for (c=tls_session_list.first; c!=NULL; c=c->nxt) {
		FD_SET (((ITEM_TLS_SESSION*)(c->item))->socket_descriptor, fds);
		n++;
		if (((ITEM_TLS_SESSION*)(c->item))->socket_descriptor > max) {
			max = ((ITEM_TLS_SESSION*)(c->item))->socket_descriptor;
		}
	}

	for (c=socks_session_list.first; c!=NULL; c=c->nxt) {
		FD_SET (((ITEM_SOCKS_SESSION*)(c->item))->target_socket_descriptor, fds);
		n++;
		if (((ITEM_SOCKS_SESSION*)(c->item))->target_socket_descriptor > max) {
			max = ((ITEM_SOCKS_SESSION*)(c->item))->target_socket_descriptor;
		}
	}

	*nfds = max + 1;
	return n;	
}



int process_porc_packet(char *buffer, int n, int tls_session_id) {
	/*ITEM_TLS_SESSION * tls_session;
	gnutls_session_t session;
	
	ChainedListFind (&tls_session_list, tls_session_id, (void**)&tls_session);
	session = tls_session->session;

	// Read the number of bytes
	int length;
	if (gnutls_record_recv (session, (char *)&length, sizeof(length))
		!= sizeof (length))
	{
		fprintf (stderr, "Impossible to read the length of the PORC packet\n");
		return -1;
	}

	// Read the remainder of the packet
	//char *buffer = malloc (length-sizeof(length));
	if (gnutls_record_recv (session, buffer, length-sizeof(length))
		!= length-sizeof (length))
	{
		fprintf (stderr, "Impossible to read the PORC packet\n");
		return -1;
	}

	int direction = ((int *)buffer)[0];		// Read the direction
	int porc_id = ((int *)buffer)[1];		// Read the PORC session

	int sock_session_id;
	ITEM_SOCKS_SESSION *sock_session;
	if (direction == PORC_DIRECTION_DOWN) {
		// We must decode

		CHAINED_LIST_LINK *c;
		for (c=socks_session_list.first; c!=NULL; c=c->nxt) {
			if ((((ITEM_SOCKS_SESSION*)(c->item))->id_prev == porc_id)// &&
				//(((ITEM_SOCKS_SESSION*)(c->item))->client_tls_session == tls_session_id))
			){
				sock_session_id = c->id;
				sock_session = (ITEM_PORC_SESSION *)(c->item);

				if (sock_session->final == 0) {
					// Decode and send to the next relay
					decodage(buffer+2*sizeof(int), ...);

				} else {
					// Decode and read the command
				}
				return 0;
			}
		}
	} else if (direction == PORC_DIRECTION_UP) {
		// We must encode

		CHAINED_LIST_LINK *c;
		for (c=socks_session_list.first; c!=NULL; c=c->nxt) {
			if ((c->id == porc_id)) &&
				(((ITEM_SOCKS_SESSION*)(c->item))->server_tls_session == tls_session_id))
			{
				sock_session_id = c->id;
				sock_session = (ITEM_SOCKS_SESSION *)(c->item);

				// Encode and send to the previous relay

				return 0
			}
		}
	} else {
		fprintf (stderr, "Incorrect direction\n");
		return -1;
	}

	fprintf (stderr, "Incorrect PORC session.\n");*/
	return -1;
}

int send_to_porc(char *buffer, int n, int socks_session_id) {
	// Send a packet from a target to a relay.


	return 0;
}

int selecting() {
	fd_set read_fds;
	int ret, nbr;
	int nfds;
	char buffer[BUF_SIZE+1];
	CHAINED_LIST_LINK *c;
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
		set_fds (&nfds, &read_fds);

		while((nbr = pselect(nfds, &read_fds, 0, 0, 0, &signal_set)) > 0) {
			for (c=porc_session_list.first; c!=NULL; c=c->nxt) {
				if (FD_ISSET (((ITEM_TLS_SESSION *)(c->item))->socket_descriptor, &read_fds)) {
					int recvd = recv(((ITEM_TLS_SESSION *)(c->item))->socket_descriptor, buffer, BUF_SIZE, 0);
					if(recvd <= 0) {
						fprintf (stderr, "Stop (100), %d\n", c->id);
						return -1;
					}
					buffer [recvd] = '\0';
					printf ("Receiving from client (%d bytes) : %s\n", recvd, buffer);
					if (process_porc_packet(buffer, recvd, c->id)!=0) {
						fprintf (stderr, "Stop (250), %d\n", c->id);
						return -1;
					}
				}
			}
			for (c=socks_session_list.first; c!=NULL; c=c->nxt) {
				if (FD_ISSET (((ITEM_SOCKS_SESSION*)(c->item))->target_socket_descriptor, &read_fds)) {
					int recvd = recv(((ITEM_SOCKS_SESSION*)(c->item))->target_socket_descriptor,
						buffer, BUF_SIZE, 0);
					if(recvd <= 0) {
						fprintf (stderr, "Stop (120), %d\n", c->id);
						return -1;
					}
					buffer [recvd] = '\0';
					printf ("Receiving from target (%d bytes) : %s\n", recvd, buffer);
					if (send_to_porc(buffer, recvd, c->id)!=0) {
						fprintf (stderr, "Stop (270), %d\n", c->id);
						return -1;
					}
				}
			}
		}
	}

	return 0;
}






/*
	main - Initializes a TLS server and starts a thread for every client.
*/
int main (int argc, char **argv)
{
	int listen_socket_descriptor;
	struct sockaddr_in sockaddr_server;
	int port;
	int ret;

	if (rsaInit()!=0) 
	{
		fprintf(stderr, "Error initializing RSA\n");
		return -1;
	}
	
	if (rsaGenKey(publicKey, privateKey)!=0) 
	{
		fprintf(stderr, "Error initializing RSA Keys\n");
		return -1;
	}
	
	if (argc != 2) {
		fprintf (stderr, "Incorrect number of argument : you must define a port to listen to\n");
		return -1;
	}
	port = atoi (argv[1]);

	if ((ret=signal_init()) != 0) {
		fprintf (stderr, "Error in signals initialisation\n");
		return -1;
	}

	if ((ret=mytls_server_init (port, &xcred, &priority_cache, &listen_socket_descriptor, 
	&sockaddr_server,1))!=0) 
	{
		fprintf (stderr, "Error in mytls_client_global_init()\n");
		return -1;
	}

	ChainedListInit (&tls_session_list);
	ChainedListInit (&porc_session_list);
	ChainedListInit (&socks_session_list);

	selecting_thread = pthread_self ();

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	ret = pthread_create(&accepting_thread, &attr, start_accepting, (void *)listen_socket_descriptor);
	if (ret != 0) {
		fprintf (stderr, "Thread creation failed\n");
		gnutls_certificate_free_credentials (xcred);
		gnutls_priority_deinit (priority_cache);
		gnutls_global_deinit ();
		return -1;
	}

	selecting ();

	ChainedListClear (&tls_session_list);
	ChainedListClear (&porc_session_list);
	ChainedListClear (&socks_session_list);

	gnutls_certificate_free_credentials (xcred);
	gnutls_priority_deinit (priority_cache);
	gnutls_global_deinit ();

	return 0;
}


