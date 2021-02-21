// Copyright 2020 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS // gethostname
#include <winsock2.h>
#elif YUZU_UNIX
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#error "Unimplemented platform"
#endif

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "core/network/network.h"
#include "core/network/sockets.h"

namespace Network {

namespace {

#ifdef _WIN32

using socklen_t = int;

void Initialize() {
    WSADATA wsa_data;
    (void)WSAStartup(MAKEWORD(2, 2), &wsa_data);
}

void Finalize() {
    WSACleanup();
}

constexpr IPv4Address TranslateIPv4(in_addr addr) {
    auto& bytes = addr.S_un.S_un_b;
    return IPv4Address{bytes.s_b1, bytes.s_b2, bytes.s_b3, bytes.s_b4};
}

sockaddr TranslateFromSockAddrIn(SockAddrIn input) {
    sockaddr_in result;

#if YUZU_UNIX
    result.sin_len = sizeof(result);
#endif

    switch (static_cast<Domain>(input.family)) {
    case Domain::INET:
        result.sin_family = AF_INET;
        break;
    default:
        UNIMPLEMENTED_MSG("Unhandled sockaddr family={}", input.family);
        result.sin_family = AF_INET;
        break;
    }

    result.sin_port = htons(input.portno);

    auto& ip = result.sin_addr.S_un.S_un_b;
    ip.s_b1 = input.ip[0];
    ip.s_b2 = input.ip[1];
    ip.s_b3 = input.ip[2];
    ip.s_b4 = input.ip[3];

    sockaddr addr;
    std::memcpy(&addr, &result, sizeof(addr));
    return addr;
}

LINGER MakeLinger(bool enable, u32 linger_value) {
    ASSERT(linger_value <= std::numeric_limits<u_short>::max());

    LINGER value;
    value.l_onoff = enable ? 1 : 0;
    value.l_linger = static_cast<u_short>(linger_value);
    return value;
}

int LastError() {
    return WSAGetLastError();
}

bool EnableNonBlock(SOCKET fd, bool enable) {
    u_long value = enable ? 1 : 0;
    return ioctlsocket(fd, FIONBIO, &value) != SOCKET_ERROR;
}

#elif YUZU_UNIX // ^ _WIN32 v YUZU_UNIX

using SOCKET = int;
using WSAPOLLFD = pollfd;
using ULONG = u64;

constexpr SOCKET INVALID_SOCKET = -1;
constexpr SOCKET SOCKET_ERROR = -1;

constexpr int WSAEWOULDBLOCK = EAGAIN;
constexpr int WSAENOTCONN = ENOTCONN;

constexpr int SD_RECEIVE = SHUT_RD;
constexpr int SD_SEND = SHUT_WR;
constexpr int SD_BOTH = SHUT_RDWR;

void Initialize() {}

void Finalize() {}

constexpr IPv4Address TranslateIPv4(in_addr addr) {
    const u32 bytes = addr.s_addr;
    return IPv4Address{static_cast<u8>(bytes), static_cast<u8>(bytes >> 8),
                       static_cast<u8>(bytes >> 16), static_cast<u8>(bytes >> 24)};
}

sockaddr TranslateFromSockAddrIn(SockAddrIn input) {
    sockaddr_in result;

    switch (static_cast<Domain>(input.family)) {
    case Domain::INET:
        result.sin_family = AF_INET;
        break;
    default:
        UNIMPLEMENTED_MSG("Unhandled sockaddr family={}", input.family);
        result.sin_family = AF_INET;
        break;
    }

    result.sin_port = htons(input.portno);

    result.sin_addr.s_addr = input.ip[0] | input.ip[1] << 8 | input.ip[2] << 16 | input.ip[3] << 24;

    sockaddr addr;
    std::memcpy(&addr, &result, sizeof(addr));
    return addr;
}

int WSAPoll(WSAPOLLFD* fds, ULONG nfds, int timeout) {
    return poll(fds, static_cast<nfds_t>(nfds), timeout);
}

int closesocket(SOCKET fd) {
    return close(fd);
}

linger MakeLinger(bool enable, u32 linger_value) {
    linger value;
    value.l_onoff = enable ? 1 : 0;
    value.l_linger = linger_value;
    return value;
}

int LastError() {
    return errno;
}

bool EnableNonBlock(int fd, bool enable) {
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) {
        return false;
    }
    if (enable) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(fd, F_SETFD, flags) == 0;
}

#endif

int TranslateDomain(Domain domain) {
    switch (domain) {
    case Domain::INET:
        return AF_INET;
    default:
        UNIMPLEMENTED_MSG("Unimplemented domain={}", domain);
        return 0;
    }
}

int TranslateType(Type type) {
    switch (type) {
    case Type::STREAM:
        return SOCK_STREAM;
    case Type::DGRAM:
        return SOCK_DGRAM;
    default:
        UNIMPLEMENTED_MSG("Unimplemented type={}", type);
        return 0;
    }
}

int TranslateProtocol(Protocol protocol) {
    switch (protocol) {
    case Protocol::TCP:
        return IPPROTO_TCP;
    case Protocol::UDP:
        return IPPROTO_UDP;
    default:
        UNIMPLEMENTED_MSG("Unimplemented protocol={}", protocol);
        return 0;
    }
}

SockAddrIn TranslateToSockAddrIn(sockaddr input_) {
    sockaddr_in input;
    std::memcpy(&input, &input_, sizeof(input));

    SockAddrIn result;

    switch (input.sin_family) {
    case AF_INET:
        result.family = Domain::INET;
        break;
    default:
        UNIMPLEMENTED_MSG("Unhandled sockaddr family={}", input.sin_family);
        result.family = Domain::INET;
        break;
    }

    result.portno = ntohs(input.sin_port);

    result.ip = TranslateIPv4(input.sin_addr);

    return result;
}

short TranslatePollEvents(PollEvents events) {
    short result = 0;

    if (True(events & PollEvents::In)) {
        events &= ~PollEvents::In;
        result |= POLLIN;
    }
    if (True(events & PollEvents::Pri)) {
        events &= ~PollEvents::Pri;
#ifdef _WIN32
        LOG_WARNING(Service, "Winsock doesn't support POLLPRI");
#else
        result |= POLLPRI;
#endif
    }
    if (True(events & PollEvents::Out)) {
        events &= ~PollEvents::Out;
        result |= POLLOUT;
    }

    UNIMPLEMENTED_IF_MSG((u16)events != 0, "Unhandled guest events=0x{:x}", (u16)events);

    return result;
}

PollEvents TranslatePollRevents(short revents) {
    PollEvents result{};
    const auto translate = [&result, &revents](short host, PollEvents guest) {
        if ((revents & host) != 0) {
            revents &= static_cast<short>(~host);
            result |= guest;
        }
    };

    translate(POLLIN, PollEvents::In);
    translate(POLLPRI, PollEvents::Pri);
    translate(POLLOUT, PollEvents::Out);
    translate(POLLERR, PollEvents::Err);
    translate(POLLHUP, PollEvents::Hup);

    UNIMPLEMENTED_IF_MSG(revents != 0, "Unhandled host revents=0x{:x}", revents);

    return result;
}

template <typename T>
Errno SetSockOpt(SOCKET fd, int option, T value) {
    const int result =
        setsockopt(fd, SOL_SOCKET, option, reinterpret_cast<const char*>(&value), sizeof(value));
    if (result != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }
    const int ec = LastError();
    UNREACHABLE_MSG("Unhandled host socket error={}", ec);
    return Errno::SUCCESS;
}

} // Anonymous namespace

NetworkInstance::NetworkInstance() {
    Initialize();
}

NetworkInstance::~NetworkInstance() {
    Finalize();
}

std::pair<IPv4Address, Errno> GetHostIPv4Address() {
    std::array<char, 256> name{};
    if (gethostname(name.data(), static_cast<int>(name.size()) - 1) == SOCKET_ERROR) {
        UNIMPLEMENTED_MSG("Unhandled gethostname error");
        return {IPv4Address{}, Errno::SUCCESS};
    }

    hostent* const ent = gethostbyname(name.data());
    if (!ent) {
        UNIMPLEMENTED_MSG("Unhandled gethostbyname error");
        return {IPv4Address{}, Errno::SUCCESS};
    }
    if (ent->h_addr_list == nullptr) {
        UNIMPLEMENTED_MSG("No addr provided in hostent->h_addr_list");
        return {IPv4Address{}, Errno::SUCCESS};
    }
    if (ent->h_length != sizeof(in_addr)) {
        UNIMPLEMENTED_MSG("Unexpected size={} in hostent->h_length", ent->h_length);
    }

    in_addr addr;
    std::memcpy(&addr, ent->h_addr_list[0], sizeof(addr));
    return {TranslateIPv4(addr), Errno::SUCCESS};
}

std::pair<s32, Errno> Poll(std::vector<PollFD>& pollfds, s32 timeout) {
    const size_t num = pollfds.size();

    std::vector<WSAPOLLFD> host_pollfds(pollfds.size());
    std::transform(pollfds.begin(), pollfds.end(), host_pollfds.begin(), [](PollFD fd) {
        WSAPOLLFD result;
        result.fd = fd.socket->fd;
        result.events = TranslatePollEvents(fd.events);
        result.revents = 0;
        return result;
    });

    const int result = WSAPoll(host_pollfds.data(), static_cast<ULONG>(num), timeout);
    if (result == 0) {
        ASSERT(std::all_of(host_pollfds.begin(), host_pollfds.end(),
                           [](WSAPOLLFD fd) { return fd.revents == 0; }));
        return {0, Errno::SUCCESS};
    }

    for (size_t i = 0; i < num; ++i) {
        pollfds[i].revents = TranslatePollRevents(host_pollfds[i].revents);
    }

    if (result > 0) {
        return {result, Errno::SUCCESS};
    }

    ASSERT(result == SOCKET_ERROR);

    const int ec = LastError();
    UNREACHABLE_MSG("Unhandled host socket error={}", ec);
    return {-1, Errno::SUCCESS};
}

Socket::~Socket() {
    if (fd == INVALID_SOCKET) {
        return;
    }
    (void)closesocket(fd);
    fd = INVALID_SOCKET;
}

Socket::Socket(Socket&& rhs) noexcept : fd{std::exchange(rhs.fd, INVALID_SOCKET)} {}

Errno Socket::Initialize(Domain domain, Type type, Protocol protocol) {
    fd = socket(TranslateDomain(domain), TranslateType(type), TranslateProtocol(protocol));
    if (fd != INVALID_SOCKET) {
        return Errno::SUCCESS;
    }

    const int ec = LastError();
    UNREACHABLE_MSG("Unhandled host socket error={}", ec);
    return Errno::SUCCESS;
}

std::pair<Socket::AcceptResult, Errno> Socket::Accept() {
    sockaddr addr;
    socklen_t addrlen = sizeof(addr);
    const SOCKET new_socket = accept(fd, &addr, &addrlen);

    if (new_socket == INVALID_SOCKET) {
        const int ec = LastError();
        UNREACHABLE_MSG("Unhandled host socket error={}", ec);
        return {AcceptResult{}, Errno::SUCCESS};
    }

    AcceptResult result;
    result.socket = std::make_unique<Socket>();
    result.socket->fd = new_socket;

    ASSERT(addrlen == sizeof(sockaddr_in));
    result.sockaddr_in = TranslateToSockAddrIn(addr);

    return {std::move(result), Errno::SUCCESS};
}

Errno Socket::Connect(SockAddrIn addr_in) {
    const sockaddr host_addr_in = TranslateFromSockAddrIn(addr_in);
    if (connect(fd, &host_addr_in, sizeof(host_addr_in)) != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }

    switch (const int ec = LastError()) {
    case WSAEWOULDBLOCK:
        LOG_DEBUG(Service, "EAGAIN generated");
        return Errno::AGAIN;
    default:
        UNREACHABLE_MSG("Unhandled host socket error={}", ec);
        return Errno::SUCCESS;
    }
}

std::pair<SockAddrIn, Errno> Socket::GetPeerName() {
    sockaddr addr;
    socklen_t addrlen = sizeof(addr);
    if (getpeername(fd, &addr, &addrlen) == SOCKET_ERROR) {
        const int ec = LastError();
        UNREACHABLE_MSG("Unhandled host socket error={}", ec);
        return {SockAddrIn{}, Errno::SUCCESS};
    }

    ASSERT(addrlen == sizeof(sockaddr_in));
    return {TranslateToSockAddrIn(addr), Errno::SUCCESS};
}

std::pair<SockAddrIn, Errno> Socket::GetSockName() {
    sockaddr addr;
    socklen_t addrlen = sizeof(addr);
    if (getsockname(fd, &addr, &addrlen) == SOCKET_ERROR) {
        const int ec = LastError();
        UNREACHABLE_MSG("Unhandled host socket error={}", ec);
        return {SockAddrIn{}, Errno::SUCCESS};
    }

    ASSERT(addrlen == sizeof(sockaddr_in));
    return {TranslateToSockAddrIn(addr), Errno::SUCCESS};
}

Errno Socket::Bind(SockAddrIn addr) {
    const sockaddr addr_in = TranslateFromSockAddrIn(addr);
    if (bind(fd, &addr_in, sizeof(addr_in)) != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }

    const int ec = LastError();
    UNREACHABLE_MSG("Unhandled host socket error={}", ec);
    return Errno::SUCCESS;
}

Errno Socket::Listen(s32 backlog) {
    if (listen(fd, backlog) != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }

    const int ec = LastError();
    UNREACHABLE_MSG("Unhandled host socket error={}", ec);
    return Errno::SUCCESS;
}

Errno Socket::Shutdown(ShutdownHow how) {
    int host_how = 0;
    switch (how) {
    case ShutdownHow::RD:
        host_how = SD_RECEIVE;
        break;
    case ShutdownHow::WR:
        host_how = SD_SEND;
        break;
    case ShutdownHow::RDWR:
        host_how = SD_BOTH;
        break;
    default:
        UNIMPLEMENTED_MSG("Unimplemented flag how={}", how);
        return Errno::SUCCESS;
    }
    if (shutdown(fd, host_how) != SOCKET_ERROR) {
        return Errno::SUCCESS;
    }

    switch (const int ec = LastError()) {
    case WSAENOTCONN:
        LOG_ERROR(Service, "ENOTCONN generated");
        return Errno::NOTCONN;
    default:
        UNREACHABLE_MSG("Unhandled host socket error={}", ec);
        return Errno::SUCCESS;
    }
}

std::pair<s32, Errno> Socket::Recv(int flags, std::vector<u8>& message) {
    ASSERT(flags == 0);
    ASSERT(message.size() < static_cast<size_t>(std::numeric_limits<int>::max()));

    const auto result =
        recv(fd, reinterpret_cast<char*>(message.data()), static_cast<int>(message.size()), 0);
    if (result != SOCKET_ERROR) {
        return {static_cast<s32>(result), Errno::SUCCESS};
    }

    switch (const int ec = LastError()) {
    case WSAEWOULDBLOCK:
        LOG_DEBUG(Service, "EAGAIN generated");
        return {-1, Errno::AGAIN};
    case WSAENOTCONN:
        LOG_ERROR(Service, "ENOTCONN generated");
        return {-1, Errno::NOTCONN};
    default:
        UNREACHABLE_MSG("Unhandled host socket error={}", ec);
        return {0, Errno::SUCCESS};
    }
}

std::pair<s32, Errno> Socket::RecvFrom(int flags, std::vector<u8>& message, SockAddrIn* addr) {
    ASSERT(flags == 0);
    ASSERT(message.size() < static_cast<size_t>(std::numeric_limits<int>::max()));

    sockaddr addr_in{};
    socklen_t addrlen = sizeof(addr_in);
    socklen_t* const p_addrlen = addr ? &addrlen : nullptr;
    sockaddr* const p_addr_in = addr ? &addr_in : nullptr;

    const auto result = recvfrom(fd, reinterpret_cast<char*>(message.data()),
                                 static_cast<int>(message.size()), 0, p_addr_in, p_addrlen);
    if (result != SOCKET_ERROR) {
        if (addr) {
            ASSERT(addrlen == sizeof(addr_in));
            *addr = TranslateToSockAddrIn(addr_in);
        }
        return {static_cast<s32>(result), Errno::SUCCESS};
    }

    switch (const int ec = LastError()) {
    case WSAEWOULDBLOCK:
        LOG_DEBUG(Service, "EAGAIN generated");
        return {-1, Errno::AGAIN};
    case WSAENOTCONN:
        LOG_ERROR(Service, "ENOTCONN generated");
        return {-1, Errno::NOTCONN};
    default:
        UNREACHABLE_MSG("Unhandled host socket error={}", ec);
        return {-1, Errno::SUCCESS};
    }
}

std::pair<s32, Errno> Socket::Send(const std::vector<u8>& message, int flags) {
    ASSERT(message.size() < static_cast<size_t>(std::numeric_limits<int>::max()));
    ASSERT(flags == 0);

    const auto result = send(fd, reinterpret_cast<const char*>(message.data()),
                             static_cast<int>(message.size()), 0);
    if (result != SOCKET_ERROR) {
        return {static_cast<s32>(result), Errno::SUCCESS};
    }

    const int ec = LastError();
    switch (ec) {
    case WSAEWOULDBLOCK:
        LOG_DEBUG(Service, "EAGAIN generated");
        return {-1, Errno::AGAIN};
    case WSAENOTCONN:
        LOG_ERROR(Service, "ENOTCONN generated");
        return {-1, Errno::NOTCONN};
    default:
        UNREACHABLE_MSG("Unhandled host socket error={}", ec);
        return {-1, Errno::SUCCESS};
    }
}

std::pair<s32, Errno> Socket::SendTo(u32 flags, const std::vector<u8>& message,
                                     const SockAddrIn* addr) {
    ASSERT(flags == 0);

    const sockaddr* to = nullptr;
    const int tolen = addr ? 0 : sizeof(sockaddr);
    sockaddr host_addr_in;

    if (addr) {
        host_addr_in = TranslateFromSockAddrIn(*addr);
        to = &host_addr_in;
    }

    const auto result = sendto(fd, reinterpret_cast<const char*>(message.data()),
                               static_cast<int>(message.size()), 0, to, tolen);
    if (result != SOCKET_ERROR) {
        return {static_cast<s32>(result), Errno::SUCCESS};
    }

    const int ec = LastError();
    UNREACHABLE_MSG("Unhandled host socket error={}", ec);
    return {-1, Errno::SUCCESS};
}

Errno Socket::Close() {
    [[maybe_unused]] const int result = closesocket(fd);
    ASSERT(result == 0);
    fd = INVALID_SOCKET;

    return Errno::SUCCESS;
}

Errno Socket::SetLinger(bool enable, u32 linger) {
    return SetSockOpt(fd, SO_LINGER, MakeLinger(enable, linger));
}

Errno Socket::SetReuseAddr(bool enable) {
    return SetSockOpt<u32>(fd, SO_REUSEADDR, enable ? 1 : 0);
}

Errno Socket::SetBroadcast(bool enable) {
    return SetSockOpt<u32>(fd, SO_BROADCAST, enable ? 1 : 0);
}

Errno Socket::SetSndBuf(u32 value) {
    return SetSockOpt(fd, SO_SNDBUF, value);
}

Errno Socket::SetRcvBuf(u32 value) {
    return SetSockOpt(fd, SO_RCVBUF, value);
}

Errno Socket::SetSndTimeo(u32 value) {
    return SetSockOpt(fd, SO_SNDTIMEO, value);
}

Errno Socket::SetRcvTimeo(u32 value) {
    return SetSockOpt(fd, SO_RCVTIMEO, value);
}

Errno Socket::SetNonBlock(bool enable) {
    if (EnableNonBlock(fd, enable)) {
        return Errno::SUCCESS;
    }
    const int ec = LastError();
    UNREACHABLE_MSG("Unhandled host socket error={}", ec);
    return Errno::SUCCESS;
}

bool Socket::IsOpened() const {
    return fd != INVALID_SOCKET;
}

} // namespace Network
