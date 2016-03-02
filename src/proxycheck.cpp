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
#include <stdarg.h> // va_start
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

//#define ADDRESS 		"127.0.0.1"
//#define PORT			631
#define REMOTE_URL		"http://www.ipchicken.com/"
#define REMOTE_HOST		"www.ipchicken.com"
#define USER_AGENT      "Mozilla/5.0 (Windows NT 6.3; Trident/7.0; rv:11.0) like Gecko"
#define CONN_TIMEOUT	5
#define THREAD_COUNT	60

int g_quietFlag = 0;

const char msg[] = "GET " REMOTE_URL " HTTP/1.1" \
    "\r\nHost: " REMOTE_HOST \
    "\r\nUser-Agent: " USER_AGENT \
	"\r\nAccept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8" \
	"\r\nConnection: keep-alive\r\n\r\n";

static size_t s_Len = 0;
static char **s_proxyList;
static pthread_mutex_t mtx;
static char * s_fileName = NULL;

/**
 * Print debug message
 */
void DebugPrint(const char *format, ...)
{
	if(g_quietFlag)
	{
		return;
	}

    va_list args;
    va_start(args, format);
    char buff[1024]; // get rid of this hard-coded buffer
    char tmp[1024];

    snprintf(tmp, 1023, "%s", format);
    vsnprintf(buff, 1023, tmp, args);
    va_end(args);

    pthread_mutex_lock(&mtx);
	printf("%s \n", buff);
	pthread_mutex_unlock(&mtx);
}

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
			*p = 0;
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
	{
		if (errno != EINPROGRESS)
		{
			return -1;
		}
	}

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
			{
				return -1;
			}
		}
		else
		{
			return -1;
		}

		if (error)
		{  //check if we had a socket error
			errno = error;
			return -1;
		}

	}
	//put socket back in blocking mode
	if (fcntl(sock, F_SETFL, flags) < 0)
	{
		DebugPrint("Error: failed to put socket in blocking mode");
		return -1;
	}

	return 0;
}

/**
 * Check proxies and report working ones
 */
void *CheckProxies(void *arg)
{
//	DebugPrint("New thread");

	char proxy[1024];
	for (size_t k = 0; k < s_Len; k++)
	{
		pthread_mutex_lock(&mtx);
		if (s_proxyList[k][0] != 0)
		{
			proxy[1024] = 0;
			strncpy(proxy, s_proxyList[k], 1023);
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

		if( strlen(proxy) < 1 )
		{
			continue;
		}

		char *port = GetPortFromIP(proxy);
		if (port == NULL)
		{
			DebugPrint("ERROR: could not get port from '%s'", proxy);
			continue;
		}

		char *endPtr = NULL;
		int portNo = (int) strtol(port, &endPtr, 10);
		if (endPtr != NULL && *endPtr != 0)
		{
			DebugPrint("ERROR: failed to convert port to number (%s)", proxy);
			continue;
		}
		char buff[1024];
		sprintf(buff, "Connecting to %s:%d", proxy, portNo);

		int sockfd = 0, n = 0;
		struct sockaddr_in serv_addr;

		if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		{
			DebugPrint("%s :: ERROR : could not create socket (errno %d)", buff, errno);
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
			DebugPrint("%s :: ERROR: failed to connect", buff);
			close(sockfd);
			continue;
		}
		else
		{
			send(sockfd, msg, strlen(msg), 0);

			char recvBuff[1200300];
			memset(recvBuff, 0, sizeof(recvBuff));
			n = 0;
			int nRead = 0;
			do
			{
				n = read(sockfd, &recvBuff[nRead], sizeof(recvBuff) - n - 1);
				if( n > 0)
				{
					nRead += n;
				}
			} while( n > 0 );

//			DebugPrint("\n\n\n\nReturned data: %s \n\n\n\n", recvBuff);

			bool success = false;
			if (strstr(recvBuff, "<title>IP Chicken"))
			{
				success = true;
			}

			if (success)
			{
				if (g_quietFlag)
				{
					printf("%s:%d\n", proxy, portNo);
				}
				else
				{
					DebugPrint("%s :: >>>>>>>>>>>>  success <<<<<<<<<<<<", buff);
				}
			}
			else
			{
				if (n < 0)
				{
					if( errno == EAGAIN)
					{
						DebugPrint("%s :: timed out (error %d) ", buff, errno);
					}
					else
					{
						DebugPrint("%s :: read error (%d) ", buff, errno);
					}
				}
				else // server returned data but not target website
				{
//					DebugPrint("Returned data: \n %s", recvBuff);
					DebugPrint("%s :: returned unexpected content ", buff);
				}
			}

			close(sockfd);
		}
	} // for
	return NULL;
}

void PrintHelp(const char * binName)
{
	printf("Usage: %s [-hq] FILE_NAME \n", binName ? binName : "proxycheck");
	printf("   -h    print this help message\n");
	printf("   -q    quiet, output only success IPs\n");
}

bool ParseArgs( int argc, char *argv[] )
{
	  int index;
	  int c;

	  opterr = 0;
	  while ((c = getopt (argc, argv, "qh")) != -1)
	    switch (c)
	      {
	      case 'h':
	        return false;
	      case 'q':
	        g_quietFlag = 1;
	        break;
	      case '?':
	        if (isprint (optopt))
	          DebugPrint("Unknown option `-%c'.\n", optopt);
	        else
	          DebugPrint("Unknown option character `\\x%x'.\n", optopt);
	        return false;
	      default:
	        return false;
	      }

	  for (index = optind; index < argc; index++)
	  {
	    s_fileName = strdup(argv[index]);
	  }

	  if( !s_fileName )
	  {
		  return false;
	  }

	  return true;
}

int main(int argc, char *argv[])
{
	if (!ParseArgs(argc, argv))
	{
		PrintHelp(argv[0]);
		return 0;
	}

	DebugPrint("Source file: '%s'", s_fileName);

	s_proxyList = ReadFile(s_fileName, &s_Len);

	DebugPrint("Number of entries: %lu ", s_Len);

	if (s_proxyList == NULL)
	{
		DebugPrint("Error: failed to read proxy file");
		return 1;
	}

	pthread_mutex_init(&mtx, NULL);

	pthread_t threads[THREAD_COUNT];
	for (size_t i = 0; i < THREAD_COUNT && i < s_Len; i++)
		pthread_create(&threads[i], NULL, &CheckProxies, NULL);

	void *status;
	for (size_t i = 0; i < THREAD_COUNT && i < s_Len; i++)
		pthread_join(threads[i], &status);

	pthread_mutex_destroy(&mtx);

	// free memory
	for (size_t i = 0; i < s_Len; i++)
		free(s_proxyList[i]);
	free(s_proxyList);

	return 0;
}
