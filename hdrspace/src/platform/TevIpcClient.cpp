#include "platform/TevIpcClient.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <Ws2tcpip.h>
#include <afunix.h>
#include <winsock2.h>
#undef NOMINMAX
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

constexpr char kOpenImageV2 = 7;

void appendBool(std::vector<char> &buffer, bool value) {
    buffer.push_back(value ? 1 : 0);
}

void appendString(std::vector<char> &buffer, const std::string &value) {
    buffer.insert(buffer.end(), value.begin(), value.end());
    buffer.push_back('\0');
}

void writePacketSize(std::vector<char> &buffer) {
    std::uint32_t size = static_cast<std::uint32_t>(buffer.size());
    std::memcpy(buffer.data(), &size, sizeof(std::uint32_t));
}

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
int closeSocket(socket_t socket) { return closesocket(socket); }
bool initSockets(std::string *errorMessage) {
    WSADATA wsaData;
    const int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != NO_ERROR) {
        if (errorMessage) *errorMessage = "WSAStartup failed.";
        return false;
    }
    return true;
}
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
int closeSocket(socket_t socket) { return close(socket); }
bool initSockets(std::string *) { return true; }
#endif

}

namespace tevipc {

bool sendOpenImage(
    const std::string &host,
    std::uint16_t port,
    const std::string &imagePath,
    const std::string &channelSelector,
    bool grabFocus,
    std::string *errorMessage
) {
    if (!initSockets(errorMessage))
        return false;

    std::vector<char> packet(sizeof(std::uint32_t), '\0');
    packet.push_back(kOpenImageV2);
    appendBool(packet, grabFocus);
    appendString(packet, imagePath);
    appendString(packet, channelSelector);
    writePacketSize(packet);

    struct addrinfo hints = {};
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *addresses = nullptr;
    const std::string portText = std::to_string(port);
    const int infoResult = getaddrinfo(host.c_str(), portText.c_str(), &hints, &addresses);
    if (infoResult != 0) {
        if (errorMessage) *errorMessage = "Could not resolve tev IPC host.";
        return false;
    }

    socket_t socketFd = kInvalidSocket;
    for (struct addrinfo *current = addresses; current; current = current->ai_next) {
        socketFd = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
        if (socketFd == kInvalidSocket)
            continue;

        if (connect(socketFd, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0)
            break;

        closeSocket(socketFd);
        socketFd = kInvalidSocket;
    }

    freeaddrinfo(addresses);

    if (socketFd == kInvalidSocket) {
        if (errorMessage) *errorMessage = "Could not connect to tev IPC.";
        return false;
    }

    size_t sent = 0;
    while (sent < packet.size()) {
#ifdef _WIN32
        const int result = send(socketFd, packet.data() + sent, static_cast<int>(packet.size() - sent), 0);
#else
        const ssize_t result = send(socketFd, packet.data() + sent, packet.size() - sent, 0);
#endif
        if (result <= 0) {
            closeSocket(socketFd);
            if (errorMessage) *errorMessage = "Could not send packet to tev.";
            return false;
        }
        sent += static_cast<size_t>(result);
    }

    closeSocket(socketFd);
    return true;
}

}
