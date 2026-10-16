// Plain UDP, no reliability or encryption: the floor every transport's overhead is measured from.
#include "driver.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>

namespace {

using spike::Channel;

int open_socket(std::uint16_t port) {
    const int socket = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
        ::close(socket);
        return -1;
    }
    return socket;
}

std::uint64_t key_of(const sockaddr_in& address) {
    return (std::uint64_t{address.sin_addr.s_addr} << 16U) | address.sin_port;
}

class Server {
public:
    bool start(std::uint16_t port) {
        socket_ = open_socket(port);
        return socket_ >= 0;
    }

    template <class Handler>
    void poll(Handler& handler) {
        std::array<std::byte, 2048> buffer{};
        sockaddr_in from{};
        socklen_t length = sizeof from;
        while (true) {
            const ssize_t got = ::recvfrom(socket_, buffer.data(), buffer.size(), 0,
                                           reinterpret_cast<sockaddr*>(&from), &length);
            if (got <= 0) {
                break;
            }
            const std::uint64_t key = key_of(from);
            if (peers_.try_emplace(key, from).second) {
                handler.connected(key);
            }
            handler.received(key, std::span{buffer.data(), static_cast<std::size_t>(got)});
        }
    }

    void send(std::uint64_t peer, Channel /*channel*/, std::span<const std::byte> bytes) {
        const sockaddr_in& to = peers_.at(peer);
        ::sendto(socket_, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&to),
                 sizeof to);
    }

    void flush() {}
    void stop() { ::close(socket_); }

private:
    int socket_ = -1;
    std::unordered_map<std::uint64_t, sockaddr_in> peers_;
};

class Clients {
public:
    bool start(int count, const std::string& host, std::uint16_t port) {
        server_.sin_family = AF_INET;
        server_.sin_port = htons(port);
        ::inet_pton(AF_INET, host.c_str(), &server_.sin_addr);
        for (int index = 0; index < count; ++index) {
            sockets_.push_back(open_socket(0));
        }
        return true;
    }

    template <class Handler>
    void poll(Handler& handler) {
        if (!announced_) {
            for (std::size_t index = 0; index < sockets_.size(); ++index) {
                handler.connected(index);
            }
            announced_ = true;
        }
        std::array<std::byte, 2048> buffer{};
        for (std::size_t index = 0; index < sockets_.size(); ++index) {
            while (true) {
                const ssize_t got = ::recv(sockets_[index], buffer.data(), buffer.size(), 0);
                if (got <= 0) {
                    break;
                }
                handler.received(index, std::span{buffer.data(), static_cast<std::size_t>(got)});
            }
        }
    }

    void send(std::uint64_t index, Channel /*channel*/, std::span<const std::byte> bytes) {
        ::sendto(sockets_[index], bytes.data(), bytes.size(), 0,
                 reinterpret_cast<const sockaddr*>(&server_), sizeof server_);
    }

    void flush() {}
    void stop() {
        for (const int socket : sockets_) {
            ::close(socket);
        }
    }

private:
    sockaddr_in server_{};
    std::vector<int> sockets_;
    bool announced_ = false;
};

} // namespace

int main(int argc, char** argv) {
    return spike::run<Server, Clients>(argc, argv);
}
