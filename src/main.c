#define _GNU_SOURCE		// kinda only need this for strcasestr
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <execinfo.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>

#include <pthread.h>
#include <bzlib.h>
#include "yxml.h"

#define BUFSIZE 2048
#define MAX_CONS 128
#define POLL_TIMEOUT 1000
#define MAX_HUB_USERS 128		// surely there won't be more than 128 users in a hub
#define MAX_SEARCH_SIZE 128

// #define DEBUG

static const char NICKNAME[] = "dbotc";
static const char DESCRIPTION[] = "There is nothing at http://172.16.122.112";
static const char VERSION[] = "0.02";
static const char MYIP[] = "172.16.122.112";
static const char ACTIVEPORT[] = "34195";
static const char WEBPORT[] = "34196";
static const char LOGFILE[] = "logs.log";
// static const char LOGLEVELSTR[][] = {"INFO", "WARN", "CRIT", "DEBUG"};

enum NodeType {HUB, CLIENT, ACTIVE, WEB, WEBCLIENT, CMD};

// Print backtrace on segfault (requires -rdynamic and -g)
void handler(int sig) {
	void *array[10];
	size_t size;

	size = backtrace(array, 10);

	fprintf(stderr, "Error: signal %d:\n", sig);
	backtrace_symbols_fd(array, size, STDERR_FILENO);
	exit(1);
}



/*		LOGGING		*/
void write_debug(char *message)
{
	FILE *fp = fopen(LOGFILE, "a");
	if (!fp) {
		perror("fopen (write_log)");
	}
	fprintf(fp, "[DEBUG](dbotc): %s\n", message);

	fclose(fp);
}


/*		SOCKET STUFF		*/

// Return a non-blocking socket that has initiated a connect() to addr and port
int tcp_setup(char *addr, char *port)
{
	int status;
	int sockfd;
	struct addrinfo hints;
	struct addrinfo *servinfo;
	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	if ((status = getaddrinfo(addr, port, &hints, &servinfo)) != 0)
	{
			fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
			return -1;
	}

	sockfd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
	if (sockfd < 0)
	{
			perror("socket");
			freeaddrinfo(servinfo);
			return -1;
	}

	// Set non-blocking sockets
	if (fcntl(sockfd, F_SETFL, O_NONBLOCK) == -1) {
		perror("fcntl");
		freeaddrinfo(servinfo);
		close(sockfd);
		return -1;
	}

	// Return socket even if still in progress (gets fed into poll() anyways)
	if (connect(sockfd, servinfo->ai_addr, servinfo->ai_addrlen) < 0)
	{
		if (errno == EINPROGRESS) {
			freeaddrinfo(servinfo);
			return sockfd;
		}
		perror("connect");
		freeaddrinfo(servinfo);
		close(sockfd);
		return -1;
	}

	freeaddrinfo(servinfo);
	return sockfd;
}

// Return a non-blocking socket that listens on PORT
int setup_tcp_server(const char *PORT)
{
	int sockfd;
	int status;
	struct addrinfo hints, *servinfo;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	if ((status = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0)
	{
			fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
			return -1;
	}

	sockfd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
	if (sockfd < 0)
	{
			perror("socket");
			return -1;
	}

	// Set non-blocking sockets
	if (fcntl(sockfd, F_SETFL, O_NONBLOCK) == -1) {
		perror("fcntl");
		freeaddrinfo(servinfo);
		close(sockfd);
		return -1;
	}

	int yes = 1;
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) == -1)
	{
			perror("setsockopt");
			return -1;
	}

	if (bind(sockfd, servinfo->ai_addr, servinfo->ai_addrlen) < 0)
	{
			perror("bind");
			return -1;
	}

	if (listen(sockfd, 20) < 0)
	{
			perror("listen");
			return -1;
	}

	freeaddrinfo(servinfo);
	return sockfd;
}

/*		SOCKET STUFF		*/


/*		UTIL		*/

bool startswith(char *start, char *haystack)
{
    return strncmp(start, haystack, strlen(start)) == 0;
}

/*		UTIL		*/


// COLD: No active connections exists
// H1, H2, H3: One of the handshake states
// CONNECTED: Connected and recognised by other end
// STREAM: In the middle of receiving streamed data
enum HandshakeState {COLD, H1, H2, H3, CONNECTED, STREAM};

typedef struct Connection {
	struct pollfd *pfd;
	bool active;

	enum NodeType type;
	void *node;

	char recvbuf[2048];
	char sendbuf[2048];
	int recvsize;
	int sendsize;
} Connection;

Connection cons[MAX_CONS];
struct pollfd *pfds;
int n_cons;


typedef struct HubUser {
	char nick[64];
	long size;
	bool active;
} HubUser;


typedef struct Hub {
	char address[64];

	enum HandshakeState state;

	char name[64];
	int usercount;		// kinda useless now that get_hub_usercount() exists
	HubUser users[MAX_HUB_USERS];
	int ts_connect;		// timestamp when last attempted connect

	long shared_size;
	time_t last_ping;
} Hub;


typedef struct Client {
	char address[65];

	enum HandshakeState state;

	char nick[64];
	Hub *hub;
	long streamsize;
	FILE *fp;
} Client;


Hub *create_hub(char *addr)
{
	Hub *hub = malloc(sizeof(Hub));
	if (!hub)
		return NULL;
	hub->address[0] = '\0';
	// strncpy(hub->address, addr, 63);
	snprintf(hub->address, 64, "%s", addr);
	// hub->address[63] = 0;
	hub->state = COLD;
	hub->name[0] = '\0';
	hub->usercount = 0;
	hub->last_ping = time(NULL);

	return hub;
}

uint32_t MurmurOAAT_32(const char* str, uint32_t h)
{
    // One-byte-at-a-time hash based on Murmur's mix
    // Source: https://github.com/aappleby/smhasher/blob/master/src/Hashes.cpp
    for (; *str; ++str) {
        h ^= *str;
        h *= 0x5bd1e995;
        h ^= h >> 15;
    }
    return h;
}

void get_filelist_name(char *nick, char *salt, char *name_buf, size_t bufsize)
{
	char pt[256];
	snprintf(pt, 256, "%s%s", nick, salt);
	uint32_t h = 0xdb07c;
	h = MurmurOAAT_32(pt, h);
	snprintf(name_buf, bufsize, "filelists/%x.xml.bz2", h);
}

// Open filelist corresponding to nick
// TODO This does not account for if different users have the same nickname on different hubs
// TODO Replace with a hash function
FILE *fopen_user_filelist(char *nick, char *mode)
{
	char filename[128];

	get_filelist_name(nick, "hubname no work :(", filename, 128);

	/*
	// Prevent path traversal vulnerabilities
	int nicksize = strlen(nick);
	for (int i = 0; i < nicksize; i++) {
		if (nick[i] == '/') {
			nick[i] = '|';
		}
	}

	snprintf(filename, 128, "filelists/%s.xml.bz2", nick);
	*/
	FILE *fp = fopen(filename, mode);
	return fp;
}

// Delete filelist corresponding to nick
// TODO This does not account for if different users have the same nickname on different hubs
bool remove_user_filelist(char *nick)
{
	char filename[128];
	get_filelist_name(nick, "hubname no work :(", filename, 128);

	/*
	// Prevent path traversal vulnerabilities
	// Better idea would maybe be to hash the nick into a string with friendlier characters?
	int nicksize = strlen(nick);
	for (int i = 0; i < nicksize; i++) {
		if (nick[i] == '/') {
			nick[i] = '|';
		}
	}

	snprintf(filename, 128, "filelists/%s.xml.bz2", nick);
	*/
	if (remove(filename) != 0) {
		char log_msg[2048];
		snprintf(log_msg, 2048, "Tried to remove file %s but it was not found.\n", filename);
		write_debug(log_msg);
		perror("remove");
		return false;
	}
	return true;
}

// returns false if the user could not be added (max user count hit)
bool add_hub_user(Connection *con, char *name, long size) {
	Hub *hub = con->node;
	bool added = false;
	// First branch checks for any unused space and immediately adds the nick
	// Second branch resolves if nick already exists
	for (int i = 0; i < MAX_HUB_USERS; i++) {
		if (!added && hub->users[i].active == false) {
			snprintf(hub->users[i].nick, 64, "%s", name);
			hub->users[i].size = size;
			hub->users[i].active = true;
			added = true;
		} else if (strncmp(hub->users[i].nick, name, 64) == 0) {
			// if the previous branch already added the nick, remove all later references
			if (added) {
				hub->users[i].active = false;
			} else {
				added = true;
			}
		}
	}
	return added;
}

// returns false if you're trying to remove a user who isn't on the hub
// BUG: DOESNT CONSIDER IF USER HAS LEFT ONLY ONE HUB OUT OF MULTIPLE (commented out part)
bool remove_hub_user(Connection *con, char *name) {
	Hub *hub = con->node;
	for (int i = 0; i < MAX_HUB_USERS; i++) {
		if (strncmp(hub->users[i].nick, name, 64) == 0) {
			hub->users[i].active = false;

			char msg[2048];
			/*
			if (remove_user_filelist(hub->users[i].nick, hub->name)) {
				snprintf(msg, 2048, "Removed %s's filelist", name );
			} else {
				snprintf(msg, 2048, "Could not remove %s's filelist", name);
			}
			write_debug(msg);
			*/

			return true;
		}
	}
	return false;
}

int get_hub_usercount(Connection *con) {
	int usercount = 0;
	Hub *hub = con->node;

	if (hub->state == COLD || con->active == false) {
		return 0;
	}

	for (int i = 0; i < MAX_HUB_USERS; i++) {
		if (hub->users[i].active) {
			usercount++;
			printf("User: %s Active?: %s\n", hub->users[i].nick, hub->users[i].active ? "True" : "False");
		}
	}
	return usercount;
}

long get_hub_shared_size(Connection *con) {
	long shared = 0;
	Hub *hub = con->node;
	for (int i = 0; i < MAX_HUB_USERS; i++) {
		if (hub->users[i].active) {
			shared += hub->users[i].size;
		}
	}
	return shared;
}

Client *create_client(char *addr, Hub *hub)
{
	Client *client = malloc(sizeof(Client));
	if (!client) {
		return NULL;
	}
	client->address[0] = '\0';
	strcpy(client->address, addr);
	client->state = COLD;
	client->nick[0] = '\0';
	client->hub = hub;
	return client;
}

bool add_connection(struct pollfd *pfds, Connection cons[], int *n_cons, enum NodeType type, void *node, int sockfd)
{
	int n = *n_cons;
	int i;
	if (*n_cons >= MAX_CONS)
	{
		printf("Connection limit %d exceeded\n", MAX_CONS);
		return false;
	}

	for (i = 0; i < *n_cons; i++) {
		if (!cons[i].active) {
			n = i;
			break;
		}
	}

	cons[n].pfd = &pfds[n];
	cons[n].pfd->fd = sockfd;
	cons[n].pfd->events |= POLLIN;	// SPEAK ONLY WHEN YOU'RE SPOKEN TO!
	cons[n].pfd->events &= ~POLLOUT;
	cons[n].active = true;
	cons[n].type = type;
	cons[n].node = node;
	cons[n].recvsize = 0;
	cons[n].sendsize = 0;

	if (n == *n_cons)		// this is really bad. n_cons needs to be reworked
		*n_cons += 1;

	return true;
}

void remove_connection(Connection *con)
{
	con->active = false;
	close(con->pfd->fd);
	con->pfd->fd = -1;
	if (con->node != NULL)
		free(con->node);
	con->node = NULL;
	con->recvsize = 0;
	con->sendsize = 0;

	// n_cons -= 1;
}

void update_events(Connection *con, short event, bool yes)
{
	if (yes)
		con->pfd->events |= event;
	else
		con->pfd->events &= event;
}


bool setup_active()
{
	int sockfd = setup_tcp_server(ACTIVEPORT);
	if (sockfd < 0) { return false; }

	add_connection(pfds, cons, &n_cons, ACTIVE, NULL, sockfd);
	printf("Active port is now listen()ing on port %s\n", ACTIVEPORT);
	return true;
}

bool setup_web()
{
	int sockfd = setup_tcp_server(WEBPORT);
	if (sockfd < 0) { return false; }

	add_connection(pfds, cons, &n_cons, WEB, NULL, sockfd);
	printf("Web server is now listen()ing on port %s\n", WEBPORT);
	return true;
}

bool consend_prepare(Connection *con, char *message, int len)
{
	if (con->sendsize + len >= BUFSIZE) {
		return false;
	}
	memcpy(con->sendbuf + con->sendsize, message, len);
	con->sendsize += len;
	// update_events(con, POLLOUT, true);
	con->pfd->events |= POLLOUT;
	return true;
}

// this function kinda only makes sense when using blocking sockets
bool consend(Connection *con)
{
	int bsent = 0;
	int btotal = 0;
	while (btotal < con->sendsize) {
		if ((bsent = send(con->pfd->fd, con->sendbuf + btotal, con->sendsize - btotal, 0)) == -1) {
			perror("send");
			return false;
		}
		btotal += bsent;
	}
	#ifdef DEBUG
		printf("[%4d]<<< %.*s\n", btotal, btotal, con->sendbuf);
	#endif
	con->sendsize = 0;
	if (con->type == WEBCLIENT) {
		// remove_connection(con);
	}
	return true;
}

bool conrecv(Connection *con)
{
	char recvbuf[BUFSIZE];
	int recvsize = recv(con->pfd->fd, recvbuf, BUFSIZE - con->recvsize - 1, 0);		// -1 is so that a null character can be added if required
																					// by some string operations later on
																					// (although, you should then just use a bigger buffer over there)
	if (recvsize <= 0)
		return false;
	#ifdef DEBUG
		printf("[%4d]>>> %.*s\n", recvsize, recvsize, recvbuf);
	#endif
	memcpy(con->recvbuf + con->recvsize, recvbuf, recvsize);
	con->recvsize += recvsize;
	return true;
}

bool hub_connect(Connection *con)
{
	Hub *hub = con->node;
	if (time(NULL) - hub->ts_connect < 30) {
		return false;
	}
	if (con->pfd->fd != -1) {
		close(con->pfd->fd);
	}
	printf("* Attempting to connect to Hub at %s\n", hub->address);
	int sockfd = tcp_setup(hub->address, "411");
	hub->ts_connect = time(NULL);

	hub->state = H1;
	con->pfd->fd = sockfd;
	con->pfd->events |= POLLIN;		// Hub sends the first message to us
	return true;
}

bool hub_action(Connection *con, char *cmd)
{

	char m[BUFSIZE];
	Hub *hub = con->node;

	printf("> [%s]: %s\n", hub->address, cmd);
	
	if (startswith("$Lock", cmd)) {
		if (hub->state != H1) { return false; }
		hub->state = H2;
		snprintf(m, BUFSIZE, "$Supports UserIP2$Key 011010110110010101111001|$ValidateNick %s|", NICKNAME);
		consend_prepare(con, m, strlen(m));
	}
	
	else if (startswith("$Hello", cmd)) {
		if (hub->state == H2) {
			hub->state = H3;
			snprintf(m, BUFSIZE, "$Version %s|$MyINFO $ALL %s %s$A$0.005A$$0$|$GetNickList|", VERSION, NICKNAME, DESCRIPTION);
			consend_prepare(con, m, strlen(m));
		} else if (hub->state == CONNECTED) {
			printf("\t * New user! Usercount: %d\n", ++hub->usercount);
		}
	}

	else if (startswith("$NickList", cmd)) {
		if (hub->state != H3) { return false; }

		char *saveptr;
		strtok_r(cmd, "$$", &saveptr);
		do {
			hub->usercount++;
		} while (strtok_r(NULL, "$$", &saveptr));

		hub->state = CONNECTED;
		printf("\t* User Count: %d\n", hub->usercount);
	}
	
	else if (startswith("$MyINFO", cmd)) {
		if (hub->state != CONNECTED) { return false; }
		char *nick;
		char *token = cmd;
		long size;

		// $MyINFO
		char *sep = strchr(cmd, ' ');
		if (!sep)
			return false;
		*sep = 0; sep++;
		if (strcmp("$MyINFO", token))
			return false;

		// $ALL
		token = sep;
		sep = strchr(sep, ' ');
		if (!sep)
			return false;
		*sep = 0; sep++;
		if (strcmp("$ALL", token))
			return false;

		// nick
		token = sep;
		sep = strchr(sep, ' ');
		if (!sep)
			return false;
		*sep = 0; sep++;
		nick = token;
		snprintf(m, BUFSIZE, "$ConnectToMe %s %s:%s|", nick, MYIP, ACTIVEPORT);

		// desc
		token = sep;
		sep = strchr(sep, '$');
		if (!sep)
			return false;
		*sep = 0; sep++;

		// active/passive
		token = sep;
		sep = strchr(sep, '$');
		if (!sep)
			return false;
		*sep = 0; sep++;

		// connection info
		token = sep;
		sep = strchr(sep, '$');
		if (!sep)
			return false;
		*sep = 0; sep++;

		// email
		token = sep;
		sep = strchr(sep, '$');
		if (!sep)
			return false;
		*sep = 0; sep++;

		// size
		token = sep;
		sep = strchr(sep, '$');
		if (!sep)
			return false;
		*sep = 0; sep++;
		if (!token) {
			size = 0;
		} else {
			size = strtol(token, NULL, 10);
		}

		add_hub_user(con, nick, size);
		printf("\t* Added User %s [%ld]\n", nick, size);
			
		printf("\t* Requested %s to $ConnectToMe\n", nick);
		consend_prepare(con, m, strlen(m));
	}

	else if (startswith("$Quit", cmd)) {
		char *saveptr;
		char *nick = strtok_r(cmd, " ", &saveptr);
		nick = strtok_r(NULL, " ", &saveptr);
		remove_hub_user(con, nick);

		printf("\t* User has left: %s Usercount: %d Shared Size: %ld\n", cmd, get_hub_usercount(con), get_hub_shared_size(con));
	}

	else if (startswith("$HubName", cmd)) {
		char *saveptr;
		char *name = strtok_r(cmd, " ", &saveptr);
		name = strtok_r(NULL, " ", &saveptr);
		if (!name) {
			return false;
		}
		strncpy(hub->name, name, 64 - 1);
		hub->name[64 - 1] = '\0';
	}

	else if (startswith("$", cmd)) {
		printf("\t ! Not implemented: %s\n", cmd);
	}
	// con->pfd->events |= POLLOUT;
	
	return true;
}

bool client_action(Connection *con, char *cmd)
{
	char m[BUFSIZE];

	Client *client = con->node;

	if (client->state == STREAM) {
		fwrite(con->recvbuf, con->recvsize, 1, client->fp);
		client->streamsize -= con->recvsize;
		con->recvsize = 0;

		if (client->streamsize <= 0) {
			client->state = CONNECTED;
			client->streamsize = 0;

			fclose(client->fp);
			client->fp = NULL;

			remove_connection(con);
			printf("* {%s}: File list tranfer completed!\n", client->nick);
		}
		return true;
	}

	printf("> {%s}: %s\n", client->nick, cmd);

	if (startswith("$MyNick", cmd)) {
		char *saveptr;
		char *nick = strtok_r(cmd, " ", &saveptr);
		nick = strtok_r(NULL, " ", &saveptr);
		strcpy(client->nick, nick);
		printf("{%s}: Check-in\n", client->nick);
	}

	else if (startswith("$Lock", cmd)) {
		snprintf(m, BUFSIZE, "$MyNick %s|$Lock EXTENDEDPROTOCOL/wut? Pk=custom-0.0.1|$Supports UserIP2 ADCGet|$Direction Download 54344|$Key ........A .....0.0. 0. 0. 0. 0. 0.|", NICKNAME);
		consend_prepare(con, m, strlen(m));
	}

	else if (startswith("$Direction", cmd)) {
		snprintf(m, BUFSIZE, "$ADCGET file files.xml.bz2 0 -1|");
		consend_prepare(con, m, strlen(m));
	}

	else if (startswith("$ADCSND", cmd)) {
		char *token, *sizestr;
		char *saveptr;
		token = strtok_r(cmd, " ", &saveptr);
		token = strtok_r(NULL, " ", &saveptr);
		token = strtok_r(NULL, " ", &saveptr);
		token = strtok_r(NULL, " ", &saveptr);
		sizestr = strtok_r(NULL, " ", &saveptr);

		long size = strtol(sizestr, NULL, 10);
		client->streamsize = size;

		FILE *fp = fopen_user_filelist(client->nick, "w");
		if (!fp) {
			fprintf(stderr, "!! Failure in fopen()\n");
			perror("fopen");
			// remove_connection(con);
			return false;
		}
		client->fp = fp;

		client->state = STREAM;
		printf("* {%s}: Transfer size %ld promised\n", client->nick, client->streamsize);
	}

	else if (startswith("$", cmd)) {
		printf("\t ! Not implemented: %s\n", cmd);
	}
	
	return true;
}

void reset_connection(Connection *con)
{
	if (con->active) {
		con->active = false;
		close(con->pfd->fd);
	}
	con->pfd = NULL;
	con->active = false;
	con->node = NULL;
	con->recvsize = 0;
	con->sendsize = 0;
	con->recvbuf[0] = 0;
	con->sendbuf[0] = 0;
}

// set hub back to cold and dark
void resethub(Connection *con) {
	Hub *hub = con->node;
	if (hub->state == CONNECTED) {
		close(con->pfd->fd);
	}
	con->pfd->fd = -1;
	con->active = false;
	con->type = HUB;
	con->recvsize = 0;
	con->sendsize = 0;
	con->recvbuf[0] = 0;
	con->sendbuf[0] = 0;

	hub->usercount = 0;
	hub->state = COLD;
	// hub->users = ????
	hub->shared_size = 0;

	for (int i = 0; i < MAX_HUB_USERS; i++) {
		hub->users[i].active = false;
	}
}

bool parsehub(Connection *con)
{
	Hub *hub = con->node;
	if (!conrecv(con)) {
		// close(con->pfd->fd);
		resethub(con);
		printf("!! [%s] Hub has disconnected\n", hub->address);
		return true;
	}

	char buf[BUFSIZE+1];
	int size = con->recvsize;
	memcpy(buf, con->recvbuf, size);
	buf[size] = '\0';

	char *saveptr;
	char *last = strrchr(buf, '|') + 1;		// this needs to go
	char *token = strtok_r(buf, "|", &saveptr);
	while (token != NULL) {
		hub_action(con, token);
		token = strtok_r(NULL, "|", &saveptr);
	}

	memcpy(con->recvbuf, last, strlen(last));
	con->recvsize = strlen(last);
	return true;
}

bool parseclient(Connection *con)
{
	Client *client = con->node;
	if (!conrecv(con)) {
		// If connect() never completed, close the previous fd before retrying
		if (client->state == H1 && con->pfd->fd)
			close(con->pfd->fd);
		client->state = COLD;
		remove_connection(con);
		printf("!! {%s}: Client has disconnected\n", client->nick);
		return true;	// rethink this
	}

	if (client->state == STREAM) {
		client_action(con, con->recvbuf);
		return true;
	}

	char buf[BUFSIZE+1];
	int size = con->recvsize;
	memcpy(buf, con->recvbuf, size);
	buf[size] = '\0';

	char *saveptr;
	char *last = buf;
	char *token = strtok_r(buf, "|", &saveptr);
	while (token != NULL) {
		last += strlen(token) + 1;
		if (!client_action(con, token)) {
			remove_connection(con);
			return false;
		}
		if (client->state == STREAM) {
			memcpy(con->recvbuf, last, con->recvsize - (last - buf));
			con->recvsize = con->recvsize - (last - buf);
			return true;
		}
		token = strtok_r(NULL, "|", &saveptr);
	}

	memcpy(con->recvbuf, last, strlen(last));
	con->recvsize = strlen(last);
	return true;
}

bool parseactive(Connection *con)
{
	struct sockaddr_storage their_addr;
	socklen_t addrsize = sizeof their_addr;

	int clientfd = accept(con->pfd->fd, (struct sockaddr *)&their_addr, &addrsize);
	if (clientfd < 0) {
		perror("accept");
		return false;
	}

	char host[64];
	char serv[64];
	/*
	if (their_addr.ss_family == AF_INET) {
		getnameinfo((struct sockaddr *)&their_addr, addrsize, host, sizeof host, serv, sizeof serv, 0);
	}
	*/
	// is this ipv6 compatible
	getnameinfo((struct sockaddr *)&their_addr, addrsize, host, sizeof host, serv, sizeof serv, 0);

	Client *client = create_client("", NULL);
	if (!client) {
		return false;
	}
	add_connection(pfds, cons, &n_cons, CLIENT, client, clientfd);
	printf("* %s connected to active port (fd: %d)! Connection count: %d\n", host, clientfd, n_cons);
	return true;
}

char *replace_encoding(char *data, int len)
{
	int i = 0;
	while (i < len) {
		if (data[i] == '%') {
			if (i+2 < len && data[i+1] == '2' && data[i+2] == '0') {
				data[i] = ' ';
				memcpy(data+i+1, data+i+3, len-i-2);
				len -= 2;
			}
		}
		if (data[i] == '+') { data[i] = ' '; }
		i++;
	}
	return data;
}

bool parsewebserv(Connection *con)
{
	struct sockaddr_storage their_addr;
	socklen_t addrsize = sizeof their_addr;

	int clientfd = accept(con->pfd->fd, (struct sockaddr *)&their_addr, &addrsize);
	if (clientfd < 0) {
		return false;
	}
	printf("* Someone connected to web port! Connection count: %d\n", n_cons);
	add_connection(pfds, cons, &n_cons, WEBCLIENT, NULL, clientfd);
	return true;
}

char *prepare_headers(char *headers, int hsize, int status, char *contenttype, long contentlength) {
	headers[0] = '\0';

	// Status code
	if (status == 200) {
		strncat(headers, "HTTP/1.1 200 OK\r\n", hsize - strlen(headers));
	}
	else {
		strncat(headers, "HTTP/1.1 404 Not Found\r\n", hsize - strlen(headers));
	}

	// Date
	char timebuf[128];
	time_t now = time(NULL);
	struct tm tm = *gmtime(&now);
	strftime(timebuf, sizeof timebuf, "Date: %a, %d %b %Y %H:%M:%S %Z\r\n", &tm);
	strncat(headers, timebuf, hsize - strlen(headers));

	// Server
	strncat(headers, "Server: dbotc/0.0.1\r\n", hsize - strlen(headers));

	// Content-Type
	char ctypebuf[128];
	snprintf(ctypebuf, sizeof ctypebuf, "Content-Type: %s\r\n", contenttype);
	strncat(headers, ctypebuf, hsize - strlen(headers));
	
	// CORS
	strncat(headers, "Access-Control-Allow-Origin: *\r\n", hsize - strlen(headers));

	// Content-Length or Transfer-Encoding
	char clenbuf[128];
	if (contentlength >= 0) {
		snprintf(clenbuf, sizeof clenbuf, "Content-Length: %ld\r\n", contentlength);
		strncat(headers, clenbuf, hsize - strlen(headers));
	}
	else {
		snprintf(clenbuf, sizeof clenbuf, "Transfer-Encoding: chunked\r\n");
		strncat(headers, clenbuf, hsize - strlen(headers));
	}

	// Additional newline
	strncat(headers, "\r\n", hsize - strlen(headers));

	return headers;
}

bool prepare_response(Connection *con, int status, char *data, long size, char *type)
{
	char response[2048] = {'\0'};
	char headers[1024] = {'\0'};
	prepare_headers(headers, sizeof headers, status, type, size);
	// this will work for string data. but binary data will probably shit itself
	snprintf(response, sizeof response, "%s%s", headers, data);
	consend_prepare(con, response, strlen(response));
	return true;
}

bool prepare_status(Connection *con)
{
	char m[1024] = {'\0'};
	// char tbuf[1024] = {'\0'};

	strncat(m, "{\"data\":[", 1024 - strlen(m));
	int hubcount = 0;
	for (int i = 0; i < MAX_CONS; i++) {
		if (cons[i].type == HUB) {
			Hub *hub = cons[i].node;
			if (hub == NULL) {
				continue;
			}
			char objbuf[1024] = {'\0'};
			snprintf(objbuf, sizeof objbuf,
					"%s{\"hub\":\"%s\", \"addr\":\"%s\", \"users\":%d, \"shared_size\":%ld, \"active\":%s}",
					hubcount == 0 ? "\0" : ",",		// JSON hates trailing commas
					hub->name,
					hub->address,
					get_hub_usercount(&cons[i]),
					get_hub_shared_size(&cons[i]),
					hub->state == CONNECTED ? "true" : "false");
			strncat(m, objbuf, sizeof(m) - strlen(m) - 1);
			hubcount++;
			printf("[DEBUG] %s %d\n", hub->address, hubcount);
		}
	}
	strncat(m, "]}", 1024 - strlen(m));
	printf("[DEBUG] %s\n", m);
	prepare_response(con, 200, m, strlen(m), "application/json");
	return true;
}


bool prepare_chunked_header(Connection *con, char *type)
{
	char headers[BUFSIZE] = {0};
	prepare_headers(headers, sizeof headers, 200, type, -1);
	return consend_prepare(con, headers, strlen(headers));
}

bool prepare_chunked_start(Connection *con)
{
	char response[64];
	snprintf(response, 64, "%x\r\n%.*s\r\n", 9, 9, "{\"data\":[");
	return consend_prepare(con, response, strlen(response));
}

bool prepare_chunked_body(Connection *con, unsigned int size, char *data)
{
	char *response = malloc(size + 64);		// should contain size of data + content-length and \r\n
	snprintf(response, size + 64, "%x\r\n%.*s\r\n", size, size, data);
	bool r = consend_prepare(con, response, strlen(response));
	free(response);
	return r;
}

bool prepare_chunked_end(Connection *con)
{
	char response[64];
	if (con->active == false) {
		fprintf(stderr, "!! Somehow the connection is not active before chunk end\n");
	}
	// JSON hates trailing commas, so this empty object just exists as the last element
	// cuz i'm too lazy to write "proper" logic for it. basically TODO but it's
	// massive and maybe not worth the effort. low budget hacks my beloved.
	// Might find some use for this in the future (error status maybe?)
	snprintf(response, 64, "%x\r\n%.*s\r\n%x\r\n\r\n", 4, 4, "{}]}", 0);
	printf("* Prepared chunk end\n");
	return consend_prepare(con, response, strlen(response));
}

bool prepare_404(Connection *con) {
	char m[] = "404 Not Found - It's not here";
	return prepare_response(con, 404, m, strlen(m), "text/plain");
}

bool prepare_error(Connection *con, char *message) {
	char m[BUFSIZE];
	snprintf(m, BUFSIZE, "{\"error\":\"%s\"}", message);
	return prepare_response(con, 200, m, strlen(m), "application/json");
}

typedef struct searchargs {
	Connection *con;
	char search[MAX_SEARCH_SIZE];
} searchargs;

// search specific file for search term
int fsearch(Connection *con, char *client_nick, char *search, char *hubname)
{
	const int MAXRESULTS = 50;
	int resultcount = 0;
	FILE *fp = fopen_user_filelist(client_nick, "rb");
	if (!fp) {
		return 1;
	}

	// setup yxml
	yxml_t *x = malloc(sizeof(yxml_t) + 4096);
	yxml_init(x, x+1, 4096);

	int bzerror;
	BZFILE *b = BZ2_bzReadOpen(&bzerror, fp, 1, 0, NULL, 0);
	if (bzerror != BZ_OK) {
		// fprintf(stderr, "!! BZ2_ReadOpen failed with bzerror: %d\n", bzerror);
		BZ2_bzReadClose(&bzerror, b);
		fclose(fp);
		free(x);
		return 1;
	}

	char buf[4096] = {0};
	bool in_file = false;
	char data[512] = {0};
	int datasize = 0;
	char path[1024] = {0};
	char chunkdata[2000];
	int dirlevel = 0;

	while ((bzerror == BZ_OK || bzerror == BZ_STREAM_END) && con->active && resultcount < MAXRESULTS) {
		int nBuf = BZ2_bzRead(&bzerror, b, buf, 4096);
		if (bzerror != BZ_OK && bzerror != BZ_STREAM_END) {
			// fprintf(stderr, "!! BZ2_bzRead failed with bzerror: %d\n", bzerror);
			BZ2_bzReadClose(&bzerror, b);
			fclose(fp);
			free(x);
			return 1;
		}
		
		for (int i = 0; i < 4096; i++) {
			yxml_ret_t r = yxml_parse(x, buf[i]);
			switch (r) {
				case YXML_ELEMSTART:
					if (strcmp(x->elem, "File") == 0) { in_file = true; }
					if (strcmp(x->elem, "Directory") == 0) { dirlevel++; }
					break;
				case YXML_ELEMEND:
					if (in_file) { in_file = false; }
					else {
						char *sep = strrchr(path, '/');
						if (sep) { *sep = 0; }
						dirlevel--;
					}
					break;
				case YXML_ATTRSTART:
					data[0] = 0;
					datasize = 0;
					break;
				case YXML_ATTRVAL:
					data[datasize] = *x->data;
					datasize++;
					break;
				case YXML_ATTREND:
					data[datasize] = 0;
					if (strcmp("TTH", x->attr) == 0) {
						break;
					}
					if (strcasestr(data, search)) {
						snprintf(chunkdata, 2000, "{\"Hub\":\"%s\",\"user\":\"%s\",\"path\":\"%s/%s\"},", hubname, client_nick, path, data);
						size_t chunkdatasize = strlen(chunkdata);
						while (!prepare_chunked_body(con, chunkdatasize, chunkdata)) {
							// fprintf(stderr, "!! Could not prepare chunk! sendbuf is probably full\n");
							;
						}
						resultcount++;
					}
					if (strcmp("Directory", x->elem) == 0 && strcmp("Name", x->attr) == 0) {
						strncat(path, "/", 1024 - strlen(path));
						strncat(path, data, 1024 - strlen(path));
					}
					break;
				default:
					break;
			}
		}
	}
	
	if (bzerror != BZ_STREAM_END) {
		// fprintf(stderr, "!! BZ2_bzRead failed with bzerror: %d\n", bzerror);
		BZ2_bzReadClose(&bzerror, b);
		fclose(fp);
		free(x);
		return 1;
	}
	else {
		BZ2_bzReadClose(&bzerror, b);
		fclose(fp);
		free(x);
		return 0;
	}
}


void filelist_search(Connection *con, char *search)
{
	printf("* Filelist search for <%s> starting...\n", search);
	for (int i = 0; i < MAX_CONS; i++) {
		if (cons[i].active && cons[i].type == HUB) {
			Hub *hub = cons[i].node;
			for (int j = 0; j < MAX_HUB_USERS; j++) {
				if (hub->users[j].active) {
					fsearch(con, hub->users[j].nick, search, hub->name);
				}
			}
		}
	}
	if (!prepare_chunked_end(con)) {
		fprintf(stderr, "Could not end chunk! (sendbuf is probably full)\n");
	}
	printf("* Filelist search for <%s> complete\n", search);
}

void *initiate_filelist_search(void *pargs)
{
	char search[MAX_SEARCH_SIZE];

	searchargs *args = pargs;
	Connection *con = args->con;
	strncpy(search, args->search, MAX_SEARCH_SIZE);
	filelist_search(con, search);

	// pargs was allocated dynamically (to prevent new threads overwriting old ones)
	free(pargs);

	return NULL;
}

void serve_static(Connection *con, char *filename)
{
	// hope and pray that the file fits in the buffer
	// prepare_response(con, 200, m, strlen(m), "text/html");
	char path[128] = {0};
	snprintf(path, 128, "pages/%s", filename);
	// FILE *fp = fopen(path, "r");
}

// perhaps this function will be the reason why you start using libraries
bool parsewebclient(Connection *con)
{
	// Parse HTTP
	if (!conrecv(con)) {
		printf("HTTP connection closed\n");
		remove_connection(con);
		return false;
	}

	char buf[BUFSIZE+1];
	int size = con->recvsize;
	memcpy(buf, con->recvbuf, size);
	buf[size] = '\0';
	con->recvsize = 0;


	char *method, *path, *version, *saveptr;
	method = strtok_r(buf, " ", &saveptr);
	if (method && strcmp("GET", method) != 0) {
		return false;
	}
	path = strtok_r(NULL, " ", &saveptr);
	if (!path) {
		return false;
	}

	// Endpoint handling
	if (strcmp("/status", path) == 0) {
		return prepare_status(con);
	}

	else if (startswith("/search/?p=", path)) {
		path = replace_encoding(path, strlen(path));
		char *sep = strchr(path, '=');
		if (!sep)
			return prepare_error(con, "Invalid query string");
		char *search = sep + 1;
		if (strlen(search) < 4) {
			return prepare_error(con, "Include at least 4 characters in your search term");
		}

		// "Hey! Please wait patiently while we sling shit at you in chunks"
		prepare_chunked_header(con, "application/json");
		prepare_chunked_start(con);

		// Create a thread for search
		pthread_t search_thread;
		searchargs *args = malloc(sizeof(searchargs));
		if (!args) {
			return prepare_error(con, "Search failed! (This is probably not your fault)");
		}
		args->con = con;
		strncpy(args->search, search, MAX_SEARCH_SIZE - 2);
		args->search[MAX_SEARCH_SIZE - 1] = 0;
		
		// goobye thread!
		int r = pthread_create(&search_thread, NULL, initiate_filelist_search, (void *)args);
		if (r != 0) {
			fprintf(stderr, "! Error in pthread_create. Did not create thread");
			return prepare_error(con, "Search failed! (This is probably not your fault)");
		}

	}
	else {
		return prepare_404(con);
	}
	return false;
}

bool parsecommand(Connection *con) {
	conrecv(con);
	char buf[BUFSIZE+1];
	memcpy(buf, con->recvbuf, con->recvsize);
	buf[con->recvsize] = '\0';

	printf("> User command[%d]: %s\n", con->recvsize, buf);
	con->recvsize = 0;
	return true;
}

bool parse(Connection *con)
{
	if (con->type == HUB) {
		return parsehub(con);
	}
	else if (con->type == CLIENT) {
		return parseclient(con);
	}
	else if (con->type == ACTIVE) {
		return parseactive(con);
	}
	else if (con->type == WEB) {
		return parsewebserv(con);
	}
	else if (con->type == WEBCLIENT) {
		return parsewebclient(con);
	}
	else if (con->type == CMD) {
		return parsecommand(con);
	}
	return false;
}

// Read hub config from hubs.cfg
// FATAL if hubs.cfg is not found
bool add_cfg_hubs()
{
	FILE *fp = fopen("hubs.cfg", "r");
	if (!fp) {
		fprintf(stderr, "!! Could not load hubs from hubs.cfg file\n");
		return false;
	}

	char buf[64];
	while (fgets(buf, 64, fp)) {
		buf[strcspn(buf, "\r\n")] = 0;		// certified binbows support moment
		if (add_connection(pfds, cons, &n_cons, HUB, create_hub(buf), -1)) {
			printf("* Hub %s added\n", buf);
		} else {
			printf("! Could not add Hub %s\n", buf);
		}
	}
	fclose(fp);
	return true;
}

// We don't really get to know if a hub has died unless you poke and ask every once in a while
bool hub_ping_check(Connection *con)
{
	char m[BUFSIZE];
	time_t now = time(NULL);
	const int DEATH_PRECISION = 30;

	Hub *hub = con->node;

	if (hub->state != CONNECTED) {
		return false;
	}

	else if (now - hub->last_ping > DEATH_PRECISION) {
		// snprintf(m, BUFSIZE, "$Ping %s:%s|", "localhost", ACTIVEPORT);
		snprintf(m, BUFSIZE, "$MyINFO $ALL %s %s$A$0.005A$$0$|", NICKNAME, DESCRIPTION);
		hub->last_ping = now;
		printf("* Pinging %s\n", hub->name);
		return consend_prepare(con, m, strlen(m));
	}
	// printf("* Wait %ld, before pinging\n", now - hub->last_ping);
	return true;
}

int main()
{
	// Turn off buffering
	setbuf(stdout, NULL);
	// Print a backtrace before segfaulting
	signal(SIGSEGV, handler);

	n_cons = 0;

	pfds = malloc(sizeof(struct pollfd) * MAX_CONS);
	if (!pfds) {
		perror("malloc");
		return 1;
	}

	for (int i = 0; i < MAX_CONS; i++) {
		reset_connection(&cons[i]);
	}

	if (!add_cfg_hubs()) {
		fprintf(stderr, "!! Could not find hubs.cfg\n");
		return 1;
	}

	if (!setup_active()) {
		fprintf(stderr, "!! Failed to setup active listener\n");
		return 1;
	}

	if (!setup_web()) {
		fprintf(stderr, "!! Failed to setup web listener\n");
		return 1;
	}
	// add_connection(pfds, cons, &n_cons, CMD, NULL, 0);

	int pcount = 0;
	do {
		if (pcount == -1) {
			perror("poll");
			continue;
		}
		for (int i = 0; i < n_cons; i++) {
			if (!cons[i].active) {
				continue;
			}
			if (cons[i].pfd->revents & POLLIN) {
				// update_events(con, POLLIN, false);
				pcount--;
				parse(&cons[i]);
			}
			if (cons[i].pfd->revents & POLLOUT) {
				// update_events(&cons[i], POLLOUT, false);
				cons[i].pfd->events &= ~POLLOUT;
				pcount--;
				if (cons[i].sendsize != 0)
					consend(&cons[i]);
			}
			if (cons[i].type == HUB && ( ((Hub *)cons[i].node)->state == COLD || ((Hub *)cons[i].node)->state == H1)) {
				hub_connect(&cons[i]);
			}
			if (cons[i].type == HUB) {
				hub_ping_check(&cons[i]);
			}
		}
	} while ((pcount = poll(pfds, n_cons, POLL_TIMEOUT) >= 0));

	free(pfds);
	// TODO: Actually free all the memory, you're going to have to move away from ctrl-c some day
	for (int i = 0; i < n_cons; i++) {
		if (cons[i].active)
			free(cons[i].node);
	}
}

/* TODO list
- Hub history / database stuff
	- Add history to web interface
- Misc chat functions
- Different people with same name on multiple hubs
 */
