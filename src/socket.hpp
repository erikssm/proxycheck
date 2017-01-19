#ifndef SOCKET_HPP_
#define SOCKET_HPP_

#include <string>
#include <unistd.h>

namespace proxycheck
{
struct Socket
{
    Socket( int fd, int timeout)
    {
        fd_ = fd;

        if( fd == -1 )
        {
            return;
        }

        // set timeout for socket read
        struct timeval tv;
        tv.tv_sec = timeout;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (char *) &tv, sizeof(struct timeval));
    }
    Socket() = delete;

    ~Socket()
    {
        if( fd_ != -1 )
        {
            ::close( fd_ );
        }
    }

    bool IsValid() const
    {
        return fd_ != -1;
    }

    operator int()
    {
        return fd_;
    }

private:
    int fd_;
};
}


#endif /* SOCKET_HPP_ */
