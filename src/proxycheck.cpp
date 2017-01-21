#include <algorithm>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h> // strerror
#include <pthread.h>
#include <stdarg.h> // va_start
#include <string>
#include <sstream>
#include <thread>
#include <sys/socket.h>
#include <vector>
#include <unistd.h>

#include "socket.hpp"

namespace
{
constexpr char REMOTE_URL[] = "http://www.ipchicken.com/";
constexpr char REMOTE_HOST[] = "www.ipchicken.com";
constexpr char USER_AGENT[] = "Mozilla/5.0 (Windows NT 6.3; Trident/7.0; rv:11.0) like Gecko";

const std::string message {
    "GET " + std::string { REMOTE_URL } + " HTTP/1.1"
    "\r\nHost: " + REMOTE_HOST + "\r\nUser-Agent: " + USER_AGENT
    + "\r\nAccept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"
    + "\r\nConnection: keep-alive" + "\r\n\r\n"
};

int CONN_TIMEOUT            = 5; // seconds
unsigned THREAD_COUNT       = 1000;

int g_quietFlag             = 0;

using ProxyList             = std::vector<std::string>;
ProxyList proxyList {};

std::mutex mtx {};
std::string proxyFile {};

/**
 * Print debug message
 */
void DebugPrint( const char *format, ... )
{
    if ( g_quietFlag )
    {
        return;
    }

    va_list args;
    va_start( args, format );
    char buff[1024]; // get rid of this hard-coded buffer
    char tmp[1024];

    snprintf( tmp, 1023, "%s", format );
    vsnprintf( buff, 1023, tmp, args );
    va_end( args );

    std::lock_guard<std::mutex> lock { mtx };
    printf( "%s \n", buff );
}

/**
 * Read proxy txt file into array
 */
ProxyList ReadFile( const std::string& name )
{
    if ( !name.length() )
    {
        return {};
    }

    ProxyList list;
    std::ifstream file( name );
    if ( !file.good() )
    {
        throw std::runtime_error { "could not open file " + name };
    }
    std::copy(
            std::istream_iterator<std::string>( file ),
            std::istream_iterator<
            std::string>(), std::back_inserter( list )
    );

    return list;
}

/**
 * returns port and removes it from string
 * ip = 192.168.1.1:44 --> 192.168.1.1
 */
inline int GetPort( std::string& ipaddr )
{
    auto n = ipaddr.find( ':' );
    if ( n == std::string::npos )
    {
        return -1;
    }

    std::string port { ipaddr.substr( n + 1 ) };
    if ( !port.length() )
    {
        return -2;
    }

    auto isNumeric = []( char c) { return !std::isdigit( c ); };
    if ( std::find_if( port.begin(), port.end(), isNumeric ) != port.end() )
    {
        return -3;
    }

    std::istringstream buffer { port };
    int value;
    buffer >> value;

    ipaddr = ipaddr.substr( 0, n );

    return value;
}

/**
 * Connect socket with timeout
 */
inline int ConnectNonBlocking( struct sockaddr_in sa, int sock, int timeout )
{
    int flags { }, error { }, result { };
    fd_set rset, wset;
    socklen_t len { sizeof(error) };
    struct timeval ts;

    ts.tv_sec   = timeout;
    ts.tv_usec  = 0;

    FD_ZERO( &rset );
    FD_SET( sock, &rset );
    wset = rset;

    if ( (flags = fcntl( sock, F_GETFL, 0 )) < 0 )
    {
        return -1;
    }

    if ( fcntl( sock, F_SETFL, flags | O_NONBLOCK ) < 0 )
    {
        return -1;
    }

    //initiate non-blocking connect
    result = connect( sock, (struct sockaddr *) &sa, 16 );
    if ( result < 0 && errno != EINPROGRESS )
    {
        return -1;
    }

    if ( result != 0 )    //then connect succeeded right away
    {
        //we are waiting for connect to complete now
        result = select( sock + 1, &rset, &wset, NULL, (timeout) ? &ts : NULL );
        if ( result < 0 )
        {
            return -1;
        }

        bool timedOut { 0 == result };
        if ( timedOut )
        {
            errno = ETIMEDOUT;
            return -1;
        }

        bool socketActivated { FD_ISSET(sock, &rset) || FD_ISSET( sock, &wset ) };
        if ( socketActivated )
        {
            if ( getsockopt( sock, SOL_SOCKET, SO_ERROR, &error, &len ) < 0 )
            {
                return -1;
            }
        }
        else
        {
            return -1;
        }

        if ( error )
        {
            errno = error;
            return -1;
        }

    }

    //put socket back in blocking mode
    if ( fcntl( sock, F_SETFL, flags ) < 0 )
    {
        DebugPrint( "Error: failed to put socket in blocking mode" );
        return -1;
    }

    return 0;
}

/**
 * Check proxies and report working ones
 */
void *CheckProxies( void *arg )
{
//	DebugPrint("New thread");

    while ( true )
    {
        std::string proxy;

        {
            std::lock_guard<std::mutex> lock { mtx };

            if ( proxyList.empty() )
            {
                break;
            }
            proxy = proxyList.front();
            proxyList.erase( proxyList.begin() );
        }

        if ( !proxy.length() )
        {
            continue;
        }

        // remove newlines, spaces
        auto replace = [&]( const std::string what )
        {
            const std::string replaceWidth = "";
            size_t start;
            while((start = proxy.find( what, start )) != std::string::npos)
            {
                proxy.replace(start, what.length(), replaceWidth );
                start += replaceWidth.length();
            }
        };
        for (auto s : { " ", "\r", "\n" } )
        {
            replace( s );
        }

        if ( !proxy.length() )
        {
            continue;
        }

        auto port = GetPort( proxy );

        if ( port < 1 )
        {
            DebugPrint( "ERROR: failed to convert port to number (%s)", proxy.c_str() );
            continue;
        }

        char buff[1024];
        snprintf( buff, 1023, "Connecting to %s:%d", proxy.c_str(), port );
        buff[sizeof(buff) - 1] = 0;

        proxycheck::Socket sockfd { socket( AF_INET, SOCK_STREAM, 0 ), CONN_TIMEOUT };
        if ( !sockfd.IsValid() )
        {
            DebugPrint( "%s :: ERROR : could not create socket (errno %d)", buff, errno );
            continue;
        }

        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons( port );
        serv_addr.sin_addr.s_addr = inet_addr( proxy.c_str() );

        if ( ConnectNonBlocking( serv_addr, sockfd, CONN_TIMEOUT ) < 0 )
        {
            DebugPrint( "%s :: ERROR: failed to connect", buff );
            continue;
        }

        send( sockfd, message.c_str(), message.length(), 0 );

        char recvBuff[1200300] { };
        int nRead { }, n;
        do
        {
            n = read( sockfd, &recvBuff[nRead], sizeof(recvBuff) - n - 1 );
            if ( n > 0 )
            {
                nRead += n;
            }
        } while ( n > 0 );

        if ( n < 0 )
        {
            switch ( errno )
            {
            case EAGAIN:
                DebugPrint( "%s :: timed out (error %d) ", buff, errno );
                break;
            default:
                DebugPrint( "%s :: read error (%d); ", buff, errno, strerror( errno ) );
            }

            continue;
        }

        if ( strstr( recvBuff, "<title>IP Chicken" ) )
        {
            if ( g_quietFlag )
            {
                printf( "%s:%d\n", proxy.c_str(), port );
            }
            else
            {
                DebugPrint( "%s :: >>>>>>>>>>>>  success <<<<<<<<<<<<", buff );
            }
        }
        else
        {
            DebugPrint( "%s :: returned unexpected content ", buff );
        }
    } // while

    return NULL;
}

void PrintHelp( const char * name )
{
    printf( "Usage: %s [-hq] FILE_NAME \n", name ? name : "proxycheck" );
    printf( "   -h    print this help message\n" );
    printf( "   -q    quiet, output only success IPs\n" );
}

bool ParseArgs( int argc, char *argv[] )
{
    int index;
    int c;

    opterr = 0;
    while ( (c = getopt( argc, argv, "qh" )) != -1 )
        switch ( c )
        {
        case 'h':
            return false;
        case 'q':
            g_quietFlag = 1;
            break;
        case '?':
            if ( isprint( optopt ) )
                DebugPrint( "Unknown option `-%c'.\n", optopt );
            else
                DebugPrint( "Unknown option character `\\x%x'.\n", optopt );
            return false;
        default:
            return false;
        }

    for (index = optind; index < argc; index++ )
    {
        proxyFile = argv[index];
    }

    return proxyFile.length() > 0;
}

} // namespace

int main( int argc, char *argv[] )
{
    try
    {
        if ( !ParseArgs( argc, argv ) )
        {
            PrintHelp( argv[0] );
            return 0;
        }

        DebugPrint( "Source file: '%s'", proxyFile.c_str() );

        proxyList = ReadFile( proxyFile );

        DebugPrint( "Number of entries: %ld ", proxyList.size() );

        if ( !proxyList.size() )
        {
            DebugPrint( "Error: failed to read proxy file" );
            return 1;
        }

        size_t listSize { proxyList.size() };
        std::thread threads[THREAD_COUNT];
        for (size_t i = 0; i < THREAD_COUNT && i < listSize; i++ )
        {
            threads[i] = std::thread{
                []() {
                    try
                    {
                        CheckProxies( NULL );
                    }
                    catch (std::exception& e)
                    {
                        std::cerr << "thread error: " << e.what() << std::endl;
                    }
                    catch ( ... )
                    {
                        std::cerr << "unknown thread error" << std::endl;
                    }
                }
            };
        }

        for (size_t i = 0; i < THREAD_COUNT && i < listSize; i++ )
        {
            threads[i].join();
        }

    }
    catch ( std::exception& ex )
    {
        std::cerr << "error: " << ex.what() << std::endl;
        return 1;
    }
    catch ( ... )
    {
        std::cerr << "unknown error" << std::endl;
        return 1;
    }

    return 0;
}
