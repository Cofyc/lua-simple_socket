#include <lua.h>
#include <lauxlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <poll.h>
#include "timeout.h"

#define SOCKET_NAME     "SOCKET*"

/* Socket */
struct socket {
    int fd;
    int sock_family;
    int sock_type;
    int sock_protocol;
    double sock_timeout;        /* in seconds */
};

#define tolsocket(L) ((struct socket *)luaL_checkudata(L, 1, SOCKET_NAME));
#define TIMEVAL_ADDSECONDS(tv, interval) \
    do { \
            tv.tv_usec += (long) ((interval - (long) interval) * 1000000); \
            tv.tv_sec += (long) interval + (long) (tv.tv_usec / 1000000); \
            tv.tv_usec %= 1000000; \
    } while (0)
#define TIMEVAL_INTERVAL(tv_start, tv_end) \
        ((tv_end.tv_sec - tv_start.tv_sec) + \
              (tv_end.tv_usec - tv_start.tv_usec) * 0.000001)
#define CHECK_ERRNO(expected)   (errno == expected)

/**
 * Function to perform the setting of socket blocking mode.
 */
static void
__setblocking(struct socket *s, int block)
{
    int flags = fcntl(s->fd, F_GETFL, 0);
    if (block) {
        flags &= (~O_NONBLOCK);
    } else {
        flags |= O_NONBLOCK;
    }
    fcntl(s->fd, F_SETFL, flags);
}

/**
 * Do a event polling on the socket, if necessary (sock_timeout > 0).
 *
 * Returns:
 *  1   on timeout
 *  -1  on error
 *  0   success
 */
#define EVENT_NONE      0
#define EVENT_READABLE  POLLIN
#define EVENT_WRITABLE  POLLOUT
#define EVENT_ANY       (POLLIN | POLLOUT)
static int
__waitfd(struct socket *s, int event, struct timeout *tm)
{
    int ret;

    // Nothing to do if we're not in timeout mode.
    if (s->sock_timeout <= 0.0)
        return 0;

    // Nothing to do if socket is closed.
    if (s->fd < 0)
        return 0;

    struct pollfd pollfd;
    pollfd.fd = s->fd;
    pollfd.events = event;

    do {
        // Handling this condition here simplifies the loops.
        double timeout = timeout_left(tm);
        if (timeout <= 0)
            return 1;
        ret = poll(&pollfd, 1, (int)(timeout * 1e3));
    } while (ret == -1 && CHECK_ERRNO(EINTR));

    if (ret < 0) {
        return -1;
    } else if (ret == 0) {
        return 1;
    } else {
        return 0;
    }
}

/**
 * Parse a socket address argument according to the socket object's address
 * family.
 * Return 1 if the address was in the proper format, 0 if not.
 *
 * Socket addresses are represented as follows:
 *  A single string is used for the AF_UNIX address family.
 *  Two arguments (host, port) is used for the AF_INET address family,
 *  where host is a string representing either a hostname in Internet Domain
 *  Notation like 'www.example.com' or an IPv4 address like '8.8.8.8', and port
 *  is an number.
 *  For AF_INET6 address family, four arguments (host, port, flowinfo, scopeid)
 *  is used, where flowinfo and scopid represents sin6_flowinfo and
 *  sin6_scope_id member in struct sockaddr_in6 in C. For socket module,
 *  flowinfo and scopid can be ommited just for backward compatibility.
 *  Other address family are currently not supported.
 *
 * The address format arguments required by a particular socket object is
 * automatically selected based on the address family specified when the socket
 * object was created.
 *
 * If you use a hostname in the host portion of IPv4/IPv6 socket address, the
 * program may show a nodeterministic behavior, as we use the first address
 * returned from the DNS resolution. The socket address will be resolved
 * differenlty into an actual IPv4/v6 address, depending on the results from DNS
 * resolution and/or the host configuration. For deterministic behavior use a
 * numeric address in host portion.
 *
 * This method assumed that address arguments start at argument index 2.
 */
static int
__getsockaddrfromarg(lua_State * L, struct socket *s, struct sockaddr *addr_ret,
                     int *len_ret)
{
    switch (s->sock_family) {
    case AF_INET:
        {
            struct sockaddr_in *addr = (struct sockaddr_in *)addr_ret;
            const char *host;
            int port;
            host = luaL_checkstring(L, 2);
            port = luaL_checknumber(L, 3);
            struct hostent *hostinfo;
            hostinfo = gethostbyname(host);
            if (hostinfo == NULL) {
                return 0;
            }
            addr->sin_family = AF_INET;
            addr->sin_port = htons(port);
            addr->sin_addr = *(struct in_addr *)hostinfo->h_addr;
            *len_ret = sizeof(*addr);
            return 1;
            break;
        }
    case AF_INET6:
        {
            break;
        }
    case AF_UNIX:
        {
            /*struct sockaddr_un *addr; */
            /*char *path; */
            /*int len; */
            break;
        }
    default:
        return 0;
    }
    return 1;
}

/**
 * Create a table, push address info into it.
 *
 * The family field os the socket object is inspected to determine what kind of
 * address it really is.
 *
 * In case of success, a table associated with address info pushed on the stack;
 * in case of error, a nil value with a string describing the error pushed.
 * Returns number of value pushed on the stack.
 */
static int
__makesockaddr(lua_State * L, struct socket *s, struct sockaddr *addr,
               socklen_t addrlen)
{
    lua_newtable(L);
    if (addrlen == 0) {
        return 1;
    }

    switch (addr->sa_family) {
    case AF_INET:
        {
            struct sockaddr_in *a = (struct sockaddr_in *)addr;
            char buf[NI_MAXHOST];
            int err = getnameinfo(addr, addrlen, buf, sizeof(buf), NULL, 0,
                                  NI_NUMERICHOST);
            if (err) {
                err = errno;
                lua_pushnil(L);
                lua_pushstring(L, gai_strerror(errno));
                return 2;
            }
            lua_pushnumber(L, 1);
            lua_pushstring(L, buf);
            lua_settable(L, -3);
            lua_pushnumber(L, 2);
            lua_pushnumber(L, ntohs(a->sin_port));
            lua_settable(L, -3);
            return 1;
        }
    default:
        lua_settop(L, 0);
        lua_pushnil(L);
        lua_pushstring(L, gai_strerror(errno));
        return 2;
    }
}

/**
 * Get the address length according to the socket object's address family.
 * Return 1 if the family is known, 0 otherwise. The length is returned through
 * len_ret.
 */
static int
__getsockaddrlen(struct socket *s, socklen_t * len_ret)
{
    switch (s->sock_family) {
    case AF_UNIX:
        *len_ret = sizeof(struct sockaddr_un);
        return 1;
    case AF_INET:
        *len_ret = sizeof(struct sockaddr_in);
        return 1;
    case AF_INET6:
        *len_ret = sizeof(struct sockaddr_in6);
        return 1;
    default:
        return 0;
    }
}

/**
 * sock, err = socket.socket(family, type[, protocol])
 *
 * Create a new socket using the given address family, socket type and protocol number.
 * The address family should be AF_INET, AF_INET6 or AF_UNIX. The
 * socket type should be SOCK_STREAM, SOCK_DGRAM or perhaps one of
 * the other SOCK_ constants. The protocol number is usually zero and may be
 * omitted in that case.
 */
static int
socket_socket(lua_State * L)
{
    int family = (int)luaL_checknumber(L, 1);
    int type = (int)luaL_checknumber(L, 2);
    int protocol = (int)luaL_optnumber(L, 3, 0);
    int fd;
    int err;
    fd = socket(family, type, protocol);
    if (fd < 0) {
        err = errno;
        lua_pushnil(L);
        lua_pushstring(L, strerror(err));
        return 2;
    }
    struct socket *s =
        (struct socket *)lua_newuserdata(L, sizeof(struct socket));
    if (!s) {
        return luaL_error(L, "out of memory");
    }
    s->fd = fd;
    s->sock_timeout = -1;
    s->sock_family = family;
    s->sock_type = type;
    s->sock_protocol = protocol;
    luaL_setmetatable(L, SOCKET_NAME);
    return 1;
}

/**
 * addrinfos, err = socket.getaddrinfo(host, port[, family=0[, socktype=0[, protocol=0[, flags=0]]]])
 *
 * Translate the host/port argument into a sequence of table that contain all
 * the necessary arguments for creating a socket to connect to that service,
 * each table has following 5 fileds:
 *  family
 *  socktype
 *  protocol
 *  canonname
 *  addr
 *
 * `host` is a domain name, a string representation of an IPv4/v6 address or nil
 * `port` is a string service name such as 'http', a numeric port number or nil
 * The `family`, `socktype`, `protocol` arguments can be optionally specified in
 * order to narrow the list of addresses returned. Passing zero (default) for
 * each of these arguments selects full range of results.
 * The `flags` can be one or serveral of the AI_* constants, and will influence
 * how results are computed and returned. For example, AI_NUMERICHOST will
 * disable domain name resolution and will raise an error if host is a domain
 * name.
 *
 */
static int
socket_getaddrinfo(lua_State * L)
{
    const char *hostname = luaL_checkstring(L, 1);
    int type = lua_type(L, 2);
    char pbuf[30];
    char *pptr;
    switch (type) {
    case LUA_TNUMBER:
        snprintf(pbuf, sizeof(pbuf), "%d", (int)luaL_checknumber(L, 2));
        pptr = pbuf;
        break;
    case LUA_TSTRING:
        pptr = (char *)luaL_checkstring(L, 2);
        break;
    case LUA_TNIL:
        pptr = (char *)NULL;
        break;
    default:
        lua_pushnil(L);
        lua_pushstring(L, "port argument expect string/number/nil");
        return 2;
        break;
    }
    int family = luaL_optnumber(L, 3, AF_UNSPEC);
    int socktype = luaL_optnumber(L, 4, 0);
    int protocol = luaL_optnumber(L, 5, 0);
    int flags = luaL_optnumber(L, 6, 0);

    struct addrinfo *iterator = NULL, *resolved = NULL;
    struct addrinfo hints;
    int ret = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = socktype;
    hints.ai_protocol = protocol;
    hints.ai_flags = flags;
    ret = getaddrinfo(hostname, pptr, &hints, &resolved);
    if (ret != 0) {
        lua_pushnil(L);
        lua_pushstring(L, gai_strerror(ret));
        return 2;
    }
    lua_newtable(L);
    int i = 1;
    for (iterator = resolved; iterator; iterator = iterator->ai_next) {
        char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
        getnameinfo(iterator->ai_addr, iterator->ai_addrlen, hbuf, sizeof(hbuf),
                    sbuf, 0, flags);
        lua_pushnumber(L, i);
        lua_newtable(L);

        lua_pushliteral(L, "family");
        lua_pushnumber(L, iterator->ai_family);
        lua_settable(L, -3);
        lua_pushliteral(L, "socktype");
        lua_pushnumber(L, iterator->ai_socktype);
        lua_settable(L, -3);
        lua_pushliteral(L, "protocol");
        lua_pushnumber(L, iterator->ai_protocol);
        lua_settable(L, -3);
        lua_pushliteral(L, "canonname");
        lua_pushstring(L, iterator->ai_canonname ? iterator->ai_canonname : "");
        lua_settable(L, -3);
        lua_pushliteral(L, "addr");
        lua_pushstring(L, hbuf);
        lua_settable(L, -3);

        lua_settable(L, -3);
        i++;
    }
    freeaddrinfo(resolved);
    return 1;
}

/**
 * ok, err = sock:connect(host, port)
 *
 * Attemps to connect to TCP socket object to a remote server.
 */
static int
sock_connect(lua_State * L)
{
    struct socket *s = tolsocket(L);
    struct sockaddr sa;
    int len;
    int err;
    err = __getsockaddrfromarg(L, s, &sa, &len);

    err = connect(s->fd, (struct sockaddr *)&sa, len);
    if (err < 0) {
        err = errno;
        lua_pushnil(L);
        lua_pushstring(L, strerror(err));
        return 2;
    }

    lua_pushboolean(L, 1);
    return 1;
}

/**
 * bytes, err = sock:send(data)
 *
 * This method is a synchronous operation that will not return until all the dat
 * a has been flushed into the system socket send buffer or an error occurs.
 *
 * In case of success, it returns the total number of bytes that have been sent.
 * Otherwise, it returns nil and a string describing the error.
 */
#define SEND_MAXSIZE 8192
static int
sock_send(lua_State * L)
{
    struct socket *s = tolsocket(L);
    size_t len, total_sent = 0;
    const char *buf = luaL_checklstring(L, 2, &len);
    char *errstr;

    struct timeout tm;
    timeout_init(&tm, s->sock_timeout);
    while (1) {
        errno = 0;
        int timeout = __waitfd(s, EVENT_WRITABLE, &tm);
        if (timeout == -1) {
            errstr = strerror(errno);
            goto err;
        } else if (timeout == 1) {
            errstr = "timed out";
            goto err;
        } else {
            size_t send_size = len - total_sent;
            if (send_size > SEND_MAXSIZE) {
                send_size = SEND_MAXSIZE;
            }
            int n = send(s->fd, buf + total_sent, send_size, 0);
            if (n < 0) {
                switch (errno) {
                case EAGAIN:
                    // do nothing, continue
                    break;
                default:
                    goto err;
                }
            } else {
                total_sent += n;
                if (len - total_sent <= 0) {
                    break;
                }
            }
        }
    }

    assert(total_sent == len);
    lua_pushinteger(L, total_sent);
    return 1;

err:
    assert(errstr);
    lua_pushnil(L);
    lua_pushstring(L, errstr);
    return 2;

}

/**
 * data, err = sock:recv(size)
 *
 * Receive data from the socket. The return value is a string representing the
 * data received. `size` specified size of data to receive, this method will not
 * return until it reads exactly size of data or an error occurs.
 *
 * In case of success, it returns the data received; in case of error, it
 * returns the partial data received if available or empty string with a estring describing the error.
 */
static int
sock_recv(lua_State * L)
{
    struct socket *s = tolsocket(L);
    size_t size = (int)luaL_checknumber(L, 2);
    char *buf = malloc(size * sizeof(char));
    size_t total_received = 0;
    char *errstr = NULL;

    struct timeout tm;
    timeout_init(&tm, s->sock_timeout);
    while (1) {
        errno = 0;
        int timeout = __waitfd(s, EVENT_WRITABLE, &tm);
        if (timeout == -1) {
            errstr = strerror(errno);
            goto err;
        } else if (timeout == 1) {
            errstr = "timed out";
            goto err;
        } else {
            int bytes =
                recv(s->fd, buf + total_received, size - total_received, 0);
            if (bytes > 0) {
                total_received += bytes;
                if (total_received == size) {
                    // finished
                    break;
                }
            } else if (bytes == 0) {
                errstr = "closed";
                goto err;
            } else {
                switch (errno) {
                case EAGAIN:
                    // do nothing, continue
                    continue;
                default:
                    errstr = strerror(errno);
                    goto err;
                }
            }
        }
    }

    assert(total_received == size);
    lua_pushlstring(L, buf, total_received);
    free(buf);
    return 1;

err:
    assert(errstr);
    lua_pushlstring(L, buf, total_received);
    free(buf);
    lua_pushstring(L, errstr);
    return 2;
}

/**
 * sock:close()
 *
 * Set the file descriptor to -1 so operations tried subsequently will surely
 * fail.
 */
static int
sock_close(lua_State * L)
{
    struct socket *s = tolsocket(L);

    int fd;

    if ((fd = s->fd) != -1) {
        s->fd = -1;
        close(fd);
    }

    return 0;
}

/**
 * ok, err = sock:shutdown(how)
 *
 * Shut down one or both halves of the connection.
 *
 * If how is SHUT_RD, further receives are disallowed.
 * If how is SHUT_WR, further sends are disallowed.
 * If how is SHUT_RDWR, further sends and receives are disallowed.
 */
static int
sock_shutdown(lua_State * L)
{
    struct socket *s = tolsocket(L);
    int how = luaL_checknumber(L, 2);
    int ret = shutdown(s->fd, how);
    if (ret < 0) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/**
 * sock:fileno()
 *
 * Return the integer file descriptor of the socket.
 */
static int
sock_fileno(lua_State * L)
{
    struct socket *s = tolsocket(L);
    lua_pushnumber(L, s->fd);
    return 1;
}

/**
 * ok, err = sock:setoption(level, optname, value)
 *
 * Set a socket option. See the Unix manual for level and option.
 */
static int
sock_setoption(lua_State * L)
{
    struct socket *s = tolsocket(L);
    int level = luaL_checknumber(L, 2);
    int optname = luaL_checknumber(L, 3);
    int flag = luaL_checknumber(L, 4);
    char *buf;
    socklen_t buflen;
    buf = (char *)&flag;
    buflen = sizeof(flag);
    int err;
    err = setsockopt(s->fd, level, optname, (void *)buf, buflen);
    if (err < 0) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/**
 * value, err = sock:getopt(level, option)
 *
 * Get a socket option. See the Unix manual for level and option.
 */
static int
sock_getoption(lua_State * L)
{
    struct socket *s = tolsocket(L);
    int level = luaL_checknumber(L, 2);
    int optname = luaL_checknumber(L, 3);
    int flag;
    int err;
    socklen_t flagsize = sizeof(flag);
    err = getsockopt(s->fd, level, optname, (void *)&flag, &flagsize);
    if (err < 0) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    lua_pushboolean(L, flag);
    return 1;
}

/**
 * sock:settimeout(timeout)
 *
 * Set the timeout value in seconds for subsequent socket operations
 * (read/recv, etc).
 *
 * A socket object can be in one of three modes: blocking, non-blocking, or
 * timeout. Sockets are by default always created in blocking mode.
 *  - In blocking mode, operations block untile complete or the system returns an
 *  error (such as connection timed out).
 *  - In non-blocking mode, operations fail (with an error that is unfortunately
 *  system-dependent) if they cannot be completed immediately.
 *  - In timeout mode, operations fail if they cannot be completed within the
 *  timeout specified for the socket or if the system returns an error. (At the
 *  os level, sockets in timeout mode are internally set in non-blocking mode.
 *
 * Argument:
 *  < 0     -- no timeout, blocking node; same as sock:setblocking(1)
 *  = 0     -- non-blocking mode; same as sock:setblocking(0)
 *  > 0     -- timeout mode (non-blocking mode)
 */
static int
sock_settimeout(lua_State * L)
{
    struct socket *s = tolsocket(L);
    double timeout = (double)luaL_checknumber(L, 2);
    s->sock_timeout = timeout;
    __setblocking(s, timeout < 0.0);
    return 0;
}

/*
 * sock:gettimeout()
 *
 * Returns the timeout in seconds (number) associated with socket.
 * A negative timeout indicates that timeout is disabled.
 */
static int
sock_gettimeout(lua_State * L)
{
    struct socket *s = tolsocket(L);
    lua_pushnumber(L, s->sock_timeout);
    return 1;
}

/**
 * sock:setblocking(block)
 *
 * 1/true   - blocking mode
 * 0/false  - non-blocking mode
 */
static int
sock_setblocking(lua_State * L)
{
    struct socket *s = tolsocket(L);
    int block = (int)luaL_checknumber(L, 2);
    s->sock_timeout = block ? -1 : 0;

    __setblocking(s, block);
    return 0;
}

/**
 * addr, err = sock:getpeername
 *
 * Return the address of the remote endpoint.
 */
static int
sock_getpeername(lua_State * L)
{
    struct socket *s = tolsocket(L);
    struct sockaddr addr;
    socklen_t addrlen;
    int ret = 0, err = 0;
    if (!__getsockaddrlen(s, &addrlen)) {
        lua_pushnil(L);
        lua_pushstring(L, "unknown address family");
        return 2;
    }
    memset(&addr, 0, addrlen);
    ret = getpeername(s->fd, &addr, &addrlen);
    if (ret < 0) {
        err = errno;
        lua_pushnil(L);
        lua_pushstring(L, strerror(err));
        return 2;
    }
    return __makesockaddr(L, s, &addr, addrlen);
}

/**
 * addr, err = sock:getsockname()
 *
 * Return the address of the local endpoint.
 */
static int
sock_getsockname(lua_State * L)
{
    struct socket *s = tolsocket(L);
    struct sockaddr addr;
    socklen_t addrlen;
    int ret = 0, err = 0;
    if (!__getsockaddrlen(s, &addrlen)) {
        lua_pushnil(L);
        lua_pushstring(L, "unknown address family");
        return 2;
    }
    memset(&addr, 0, addrlen);
    ret = getsockname(s->fd, &addr, &addrlen);
    if (ret < 0) {
        err = errno;
        lua_pushnil(L);
        lua_pushstring(L, strerror(err));
        return 2;
    }
    return __makesockaddr(L, s, &addr, addrlen);
}

static int
__sock_tostring(lua_State * L)
{
    struct socket *s = tolsocket(L);
    lua_pushfstring(L, "<socket: %d>", s->fd);
    return 1;
}

static const luaL_Reg socketlib[] = {
    {"socket", socket_socket},
    {"getaddrinfo", socket_getaddrinfo},
    {NULL, NULL},
};

static const luaL_Reg sock_methods[] = {
    {"__gc", sock_close},
    {"__tostring", __sock_tostring},
    {"connect", sock_connect},
    {"send", sock_send},
    {"recv", sock_recv},
    {"close", sock_close},
    {"shutdown", sock_shutdown},
    {"fileno", sock_fileno},
    {"setoption", sock_setoption},
    {"getoption", sock_getoption},
    {"settimeout", sock_settimeout},
    {"gettimeout", sock_gettimeout},
    {"setblocking", sock_setblocking},
    {"getpeername", sock_getpeername},
    {"getsockname", sock_getsockname},
    {NULL, NULL},
};

int
luaopen_socket_c(lua_State * L)
{
    luaL_checkversion(L);
    luaL_newlib(L, socketlib);

#define ADD_NUM_CONST(name)     \
    lua_pushnumber(L, name);  \
    lua_setfield(L, -2, # name)

    // These constants represent the address (and protocol) families, used for the first argument to socket().
    ADD_NUM_CONST(AF_INET);
    ADD_NUM_CONST(AF_INET6);
    ADD_NUM_CONST(AF_UNIX);

    // These constants represent the socket types, used for the second argument to socket(). (Only SOCK_STREAM and SOCK_DGRAM appear to be generally useful.)
    ADD_NUM_CONST(SOCK_STREAM);
    ADD_NUM_CONST(SOCK_DGRAM);
    ADD_NUM_CONST(SOCK_RAW);
    ADD_NUM_CONST(SOCK_RDM);
    ADD_NUM_CONST(SOCK_SEQPACKET);

    // socket.SOMAXCONN
    ADD_NUM_CONST(SOMAXCONN);

    // AI_*
    ADD_NUM_CONST(AI_PASSIVE);
    ADD_NUM_CONST(AI_CANONNAME);
    ADD_NUM_CONST(AI_NUMERICHOST);
    ADD_NUM_CONST(AI_NUMERICSERV);
    ADD_NUM_CONST(AI_MASK);
    ADD_NUM_CONST(AI_ALL);
    ADD_NUM_CONST(AI_V4MAPPED_CFG);
    ADD_NUM_CONST(AI_ADDRCONFIG);
    ADD_NUM_CONST(AI_V4MAPPED);
    ADD_NUM_CONST(AI_DEFAULT);

    // INADDR_* Some reserved IPv4 addresses
    ADD_NUM_CONST(INADDR_ANY);
    ADD_NUM_CONST(INADDR_BROADCAST);
    ADD_NUM_CONST(INADDR_LOOPBACK);
    ADD_NUM_CONST(INADDR_UNSPEC_GROUP);
    ADD_NUM_CONST(INADDR_ALLHOSTS_GROUP);
    ADD_NUM_CONST(INADDR_MAX_LOCAL_GROUP);
    ADD_NUM_CONST(INADDR_NONE);

    // TCP_* Tcp options
    ADD_NUM_CONST(TCP_NODELAY);
    ADD_NUM_CONST(TCP_KEEPALIVE);

    // SHUT_* sock:shutdown() parameters
    ADD_NUM_CONST(SHUT_RD);
    ADD_NUM_CONST(SHUT_WR);
    ADD_NUM_CONST(SHUT_RDWR);

    // Create a metatable for socket userdata.
    luaL_newmetatable(L, SOCKET_NAME);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");     /* metable.__index = metatable */
    luaL_setfuncs(L, sock_methods, 0);
    lua_pop(L, 1);

    // install a handler to ignore sigpipe or it will crash us
    signal(SIGPIPE, SIG_IGN);

    return 1;
}
