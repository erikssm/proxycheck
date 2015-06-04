#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>

#define ADDRESS 		"127.0.0.1"
#define PORT			631
#define REMOTE_HOST		"http://www.ipchicken.com"
#define CONN_TIMEOUT	2
#define THREAD_COUNT	30

const char msg[] = "GET " REMOTE_HOST "/ HTTP/1.1\r\nHost: " REMOTE_HOST "\r\nConnection: close\r\n\r\n";

static size_t s_Len = 0;
static char **s_proxyList;
static pthread_mutex_t mtx;

/**
 * Read proxy txt file into array
 */
char **ReadFile(const char *name, size_t *size)
{
	char **out = (char **) malloc(sizeof(char **) * 100);
	out[0] = NULL;

	if (name == NULL)
		return NULL;

	FILE * fp;
	size_t len = 0;
	ssize_t read;

	fp = fopen(name, "r");
	if (fp == NULL)
		return NULL;

	int i = 0;
	while ((read = getline(&out[i], &len, fp)) != -1)
	{
		i++;
		if (i % 100 == 0)
			out = (char **) realloc(out, sizeof(char **) * (i + 100));
		out[i] = NULL;
	}
	*size = i;

	fclose(fp);

	return out;
}

/**
 * replaces colon with 0 and returns pointer to port
 * ip = 192.168.1.1:44 --> 192.168.1.1\044
 */
inline char *GetPortFromIP(char *ipStr)
{
	if (ipStr == NULL)
		return NULL;

	char *p = ipStr;
	while (*p != 0)
	{
		if (*p == ':')
		{
			*p = 0x0;
			return ++p;
		}
		p++;
	}
	return NULL;
}

/**
 * Connect socket with timeout
 */
inline int ConnectNonBlocking(struct sockaddr_in sa, int sock, int timeout)
{
	int flags = 0, error = 0, ret = 0;
	fd_set rset, wset;
	socklen_t len = sizeof(error);
	struct timeval ts;

	ts.tv_sec = timeout;
	ts.tv_usec = 0;

	//clear out descriptor sets for select
	//add socket to the descriptor sets
	FD_ZERO(&rset);
	FD_SET(sock, &rset);
	wset = rset;    //structure assignment ok

	//set socket nonblocking flag
	if ((flags = fcntl(sock, F_GETFL, 0)) < 0)
		return -1;

	if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;

	//initiate non-blocking connect
	if ((ret = connect(sock, (struct sockaddr *) &sa, 16)) < 0)
		if (errno != EINPROGRESS)
			return -1;

	if (ret != 0)    //then connect succeeded right away
	{
		//we are waiting for connect to complete now
		if ((ret = select(sock + 1, &rset, &wset, NULL, (timeout) ? &ts : NULL)) < 0)
			return -1;
		if (ret == 0)
		{   //we had a timeout
			errno = ETIMEDOUT;
			return -1;
		}

		//we had a positivite return so a descriptor is ready
		if (FD_ISSET(sock, &rset) || FD_ISSET(sock, &wset))
		{
			if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
				return -1;
		}
		else
			return -1;

		if (error)
		{  //check if we had a socket error
			errno = error;
			return -1;
		}

	}
	//put socket back in blocking mode
	if (fcntl(sock, F_SETFL, flags) < 0)
	{
		printf("Error: failed to put socket in blocking mode");
		return -1;
	}

	return 0;
}

/**
 * Check proxies and report working ones
 */
void *CheckProxies(void *arg)
{
	char proxy[1024];
	for (size_t k = 0; k < s_Len; k++)
	{
		pthread_mutex_lock(&mtx);
		if (s_proxyList[k][0] != 0)
		{
			strcpy(proxy, s_proxyList[k]);
			s_proxyList[k][0] = 0;
			pthread_mutex_unlock(&mtx);
		}
		else
		{
			pthread_mutex_unlock(&mtx);
			continue;
		}

		// remove newline
		char *pos = strchr(proxy, '\n');
		if (pos != NULL)
		{
			*pos = '\0';
			pos--;
			if (*pos == '\r')
				*pos = '\0';
		}

		char *port = GetPortFromIP(proxy);
		if (port == NULL)
			continue;

		char *endPtr = NULL;
		int portNo = (int) strtol(port, &endPtr, 10);
		if (endPtr != NULL && *endPtr != 0)
		{
			printf("Error: failed to convert port to number (%s)\n", proxy);
			continue;
		}
		char buff[1024];
		sprintf(buff, "Connecting to %s:%d ", proxy, portNo);

		int sockfd = 0, n = 0;
		char recvBuff[1024];
		struct sockaddr_in serv_addr;

		if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		{
			printf("%s :: Error : could not create socket (errno %d)\n", buff, errno);
			continue;
		}

		// set timeout for socket read
		struct timeval tv;
		tv.tv_sec = CONN_TIMEOUT;
		tv.tv_usec = 0;
		setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char *) &tv, sizeof(struct timeval));

		serv_addr.sin_family = AF_INET;
		serv_addr.sin_port = htons(portNo);
		serv_addr.sin_addr.s_addr = inet_addr(proxy);

		int err = ConnectNonBlocking(serv_addr, sockfd, CONN_TIMEOUT);

		if (err < 0)
		{
			printf("%s :: Error: failed to connect\n", buff);
			close(sockfd);
			continue;
		}
		else
		{
			send(sockfd, msg, strlen(msg), 0);

			memset(recvBuff, '0', sizeof(recvBuff));
			n = read(sockfd, recvBuff, sizeof(recvBuff) - 1);
			if ( n == EAGAIN ) // try again
				n = read(sockfd, recvBuff, sizeof(recvBuff) - 1);

			bool response = false;
			while (n > 0)
			{
				recvBuff[n] = 0;
				if (strstr(recvBuff, "<title>IP Chicken"))
					response = true;

				n = read(sockfd, recvBuff, sizeof(recvBuff) - 1);
			}
			if (response)
				printf("%s :: success\n", buff);

			if (n < 0)
				printf("%s :: read error (%d) \n", buff, errno);

			close(sockfd);
		}
	} // for
	return NULL;
}

int main(int argc, char *argv[])
{
	if (argc != 2)
	{
		printf("Usage: proxycheck <proxylist.txt>\n");
		return 0;
	}

	s_proxyList = ReadFile(argv[1], &s_Len);

	if (s_proxyList == NULL)
	{
		printf("Error: failed to read proxy file\n");
		return 1;
	}

	pthread_mutex_init(&mtx, NULL);

	pthread_t threads[THREAD_COUNT];
	for (int i = 0; i < THREAD_COUNT; i++)
		pthread_create(&threads[i], NULL, &CheckProxies, NULL);

	void *status;
	for (int i = 0; i < THREAD_COUNT; i++)
		pthread_join(threads[i], &status);

	pthread_mutex_destroy(&mtx);

	// free memory
	for (size_t i = 0; i < s_Len; i++)
		free(s_proxyList[i]);
	free(s_proxyList);

	return 0;
}
