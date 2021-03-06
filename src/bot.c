#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <dlfcn.h>
#include <dirent.h>
#include <unistd.h>
#include "bot.h"

int keep_alive = 1;

int getaddr(struct addrinfo **result);
int connect_to_server();
int send_msg(int, struct irc_message *message);
struct irc_message *recv_msg(int);

void free_message(struct irc_message *message);
void process_message(int, struct irc_message *msg);
int filter(const struct dirent *);

struct plugin {
	void *handle;
	char *command;
	int (*create_response) (struct irc_message *, struct irc_message **,
				int *);
	int (*initialize) ();
	int (*close) ();
};

static struct plugin plugins[MAX_PLUGINS];
static int nplugins = 0;

int filter(const struct dirent *d)
{
	int len;

	if (strcmp(d->d_name, "..") == 0 || strcmp(d->d_name, ".") == 0)
		return 0;

	len = strlen(d->d_name);

	if (len < 4)
		return 0;

	if (strcmp(d->d_name + len - 3, ".so") == 0)
		return 1;

	return 0;
}

void load_plugins()
{
	struct dirent **namelist;
	void *handle;
	int n;

	n = scandir("plugins", &namelist, &filter, alphasort);
	if (n < 0) {
		perror("scandir");
		exit(EXIT_FAILURE);
	}

	while (n--) {

		char location[100] = "plugins/";
		strcpy(location + 8, namelist[n]->d_name);

		handle = dlopen(location, RTLD_LAZY);

		if (!handle) {
			fprintf(stderr, "%s\n", dlerror());
			exit(EXIT_FAILURE);
		}

		plugins[nplugins].handle = handle;

		plugins[nplugins].command = (char *)dlsym(handle, "command");
		plugins[nplugins].create_response =
		    dlsym(handle, "create_response");
		plugins[nplugins].initialize = dlsym(handle, "initialize");
		plugins[nplugins].close = dlsym(handle, "close");

		/* only count this as a valid plugin if both create_response
		 * and command were found */
		if (plugins[nplugins].create_response
		    && plugins[nplugins].command)
			nplugins++;

		free(namelist[n]);
	}

	free(namelist);
}

void run_bot()
{
	int p_index;
	struct irc_message *inc_msg;
	int sockfd;

	load_plugins();

	for (p_index = 0; p_index < nplugins; p_index++) {
		if (plugins[p_index].initialize)
			plugins[p_index].initialize();
	}

	sockfd = connect_to_server();

	do {
		inc_msg = recv_msg(sockfd);
		if(!inc_msg)
			break;
		process_message(sockfd, inc_msg);
		free_message(inc_msg);
	} while (keep_alive);

	close(sockfd);

	for (p_index = 0; p_index < nplugins; p_index++) {
		if (plugins[p_index].close) {
			plugins[p_index].close();
		}
		dlclose(plugins[p_index].handle);
	}
}

void process_message(int sockfd, struct irc_message *msg)
{
	int i;
	struct irc_message *responses[MAX_RESPONSE_MSGES];
	int num_of_responses = 0;

	for (i = 0; i < nplugins; i++) {
		struct irc_message *temp_msg;

		if (strcmp(plugins[i].command, msg->command))
			continue;

		temp_msg =
		    create_message(msg->prefix, msg->command, msg->params);
		plugins[i].create_response(temp_msg, responses,
					   &num_of_responses);
		free_message(temp_msg);

		if (num_of_responses > 0) {
			int i;
			for (i = 0; i < num_of_responses; i++) {
				send_msg(sockfd, responses[i]);
				free_message(responses[i]);
			}
		}
	}
}

int connect_to_server()
{
	struct addrinfo *addr;
	int sockfd;
	int conn_result;

	getaddr(&addr);

	sockfd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
	if (sockfd < 0) {
		perror("socket");
		exit(EXIT_FAILURE);
	}

	conn_result = connect(sockfd, addr->ai_addr, addr->ai_addrlen);
	if (conn_result < 0) {
		fprintf(stderr, "Failure connecting to %s", address);
		return -1;
	}
	freeaddrinfo(addr);

	return sockfd;
}

int send_msg(int sockfd, struct irc_message *message)
{
	char buf[IRC_BUF_LENGTH];
	int idx = 0;

	if (message->prefix) {
		sprintf(buf + idx, "%s ", message->prefix);
		idx += strlen(message->prefix) + 1;
	}

	sprintf(buf + idx, "%s", message->command);
	idx += strlen(message->command);

	if (message->params) {
		sprintf(buf + idx, " %s", message->params);
		idx += strlen(message->params) + 1;
	}

	sprintf(buf + idx, "\r\n");

	/*printf("S:%s", buf); */

	return send(sockfd, buf, strlen(buf), 0);
}

struct irc_message *recv_msg(int sockfd)
{
	char buf[IRC_BUF_LENGTH];
	int bytes_rcved = 0;
	char *prefix, *command, *params, *tok;
	struct irc_message *msg;

	do {
		int bytes_read;
		bytes_read = recv(sockfd, buf + bytes_rcved, 1, 0);
		if (bytes_read == 0) {
			fprintf(stderr, "Connection closed.\n");
			return NULL;
		}

		if (bytes_read == -1) {
			perror("recv");
			return NULL;
		}

		bytes_rcved += bytes_read;
	} while (buf[bytes_rcved - 1] != '\n');

	buf[bytes_rcved] = '\0';

	prefix = command = params = NULL;

	tok = strtok(buf, " ");
	if (tok[0] != ':') {
		command = tok;
	} else {
		prefix = tok;
		command = strtok(NULL, " ");
	}

	if ((tok = strtok(NULL, "\r\n")) != NULL) {
		params = tok;
	}

	msg = create_message(prefix, command, params);

	return msg;
}

struct irc_message *create_message(char *prefix, char *command, char *params)
{
	struct irc_message *msg;

	msg = malloc(sizeof(struct irc_message));
	msg->prefix = NULL;
	msg->command = NULL;
	msg->params = NULL;

	if (prefix)
		msg->prefix = strdup(prefix);

	msg->command = strdup(command);

	if (params)
		msg->params = strdup(params);

	return msg;
}

void free_message(struct irc_message *message)
{
	if (message->prefix)
		free(message->prefix);
	if (message->params)
		free(message->params);
	if (message->command)
		free(message->command);
	free(message);
}

int getaddr(struct addrinfo **result)
{
	char *name, *port, *addr;
	struct addrinfo hints;
	int s;

	addr = strdup(address);
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = 0;
	hints.ai_protocol = 0;

	name = strtok(addr, ":");
	port = strtok(NULL, " ");

	s = getaddrinfo(name,
			port == NULL ? DEFAULT_PORT : port, &hints, result);
	free(addr);

	if (s != 0) {
		perror("addrinfo\n");
		exit(EXIT_FAILURE);
	}

	return 0;
}
