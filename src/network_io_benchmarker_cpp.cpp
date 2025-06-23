#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <memory>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <future>
#include <random>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <map>
#include <functional>
#include <optional>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <cstring>

#ifdef __APPLE__
#include <sys/event.h>
#include <sys/time.h>
#else
#include <sys/epoll.h>
#endif

namespace NetworkIO {

using Clock = std::chrono::high_resolution_clock;
using Duration = std::chrono::duration<double>;
using TimePoint = Clock::time_point;

constexpr int DEFAULT_PORT = 12345;
constexpr size_t DEFAULT_MESSAGE_SIZE = 1024;
constexpr int DEFAULT_NUM_MESSAGES = 10000;
constexpr int DEFAULT_NUM_CONNECTIONS = 100;
constexpr int MAX_EVENTS = 1024;
constexpr int MAX_CONNECTIONS = 10000;
constexpr size_t HISTOGRAM_BUCKETS = 50;
constexpr size_t KB = 1024;
constexpr size_t MB = 1024 * 1024;

enum class Protocol {
    TCP,
    UDP
};

enum class IOMultiplexer {
    Select,
    Poll,
    Epoll,
    Kqueue
};

enum class TestType {
    Throughput,
    Latency,
    BufferSize,
    Multiplexer,
    ConnectionScaling,
    All
};

enum class Mode {
    Client,
    Server,
    Both
};

struct NetworkStats {
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> messages_sent{0};
    std::atomic<uint64_t> messages_received{0};
    std::atomic<double> total_time{0.0};
    std::atomic<double> min_latency{std::numeric_limits<double>::infinity()};
    std::atomic<double> max_latency{0.0};
    std::atomic<double> total_latency{0.0};
    std::atomic<uint64_t> latency_count{0};
    
    std::vector<double> latency_samples;
    std::mutex latency_mutex;
    
    void add_latency_sample(double latency) {
        // Atomic addition for double using compare-and-swap
        double current_total = total_latency.load(std::memory_order_relaxed);
        while (!total_latency.compare_exchange_weak(current_total, current_total + latency, std::memory_order_relaxed)) {
            // Keep trying
        }
        latency_count.fetch_add(1, std::memory_order_relaxed);
        
        // Update min latency
        double current_min = min_latency.load(std::memory_order_relaxed);
        while (latency < current_min && 
               !min_latency.compare_exchange_weak(current_min, latency, std::memory_order_relaxed)) {
            // Keep trying
        }
        
        // Update max latency
        double current_max = max_latency.load(std::memory_order_relaxed);
        while (latency > current_max && 
               !max_latency.compare_exchange_weak(current_max, latency, std::memory_order_relaxed)) {
            // Keep trying
        }
        
        // Store sample for percentile calculation
        std::lock_guard<std::mutex> lock(latency_mutex);
        latency_samples.push_back(latency);
    }
    
    std::vector<double> get_percentiles() const {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(latency_mutex));
        if (latency_samples.empty()) return {};
        
        std::vector<double> sorted_samples = latency_samples;
        std::sort(sorted_samples.begin(), sorted_samples.end());
        
        std::vector<double> percentiles;
        std::vector<double> p_values = {0.5, 0.75, 0.9, 0.95, 0.99, 0.999};
        
        for (double p : p_values) {
            size_t index = static_cast<size_t>(p * sorted_samples.size());
            if (index >= sorted_samples.size()) index = sorted_samples.size() - 1;
            percentiles.push_back(sorted_samples[index]);
        }
        
        return percentiles;
    }
};

class SocketWrapper {
private:
    int fd_;
    bool owner_;
    
public:
    SocketWrapper() : fd_(-1), owner_(false) {}
    
    explicit SocketWrapper(int fd, bool owner = true) : fd_(fd), owner_(owner) {}
    
    SocketWrapper(SocketWrapper&& other) noexcept : fd_(other.fd_), owner_(other.owner_) {
        other.fd_ = -1;
        other.owner_ = false;
    }
    
    SocketWrapper& operator=(SocketWrapper&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            owner_ = other.owner_;
            other.fd_ = -1;
            other.owner_ = false;
        }
        return *this;
    }
    
    ~SocketWrapper() {
        close();
    }
    
    void close() {
        if (fd_ >= 0 && owner_) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    
    int get() const { return fd_; }
    int release() { owner_ = false; return fd_; }
    bool is_valid() const { return fd_ >= 0; }
    
    static SocketWrapper create_tcp() {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            throw std::runtime_error("Failed to create TCP socket: " + std::string(strerror(errno)));
        }
        return SocketWrapper(fd);
    }
    
    static SocketWrapper create_udp() {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            throw std::runtime_error("Failed to create UDP socket: " + std::string(strerror(errno)));
        }
        return SocketWrapper(fd);
    }
    
    void set_nonblocking() {
        int flags = fcntl(fd_, F_GETFL, 0);
        if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
            throw std::runtime_error("Failed to set non-blocking: " + std::string(strerror(errno)));
        }
    }
    
    void set_reuse_addr() {
        int opt = 1;
        if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            throw std::runtime_error("Failed to set SO_REUSEADDR: " + std::string(strerror(errno)));
        }
    }
    
    void set_buffer_size(int size) {
        if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0 ||
            setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0) {
            throw std::runtime_error("Failed to set buffer size: " + std::string(strerror(errno)));
        }
    }
    
    void set_tcp_nodelay() {
        int nodelay = 1;
        if (setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
            throw std::runtime_error("Failed to set TCP_NODELAY: " + std::string(strerror(errno)));
        }
    }
};

struct ConnectionInfo {
    SocketWrapper socket;
    sockaddr_in addr;
    uint64_t bytes_sent{0};
    uint64_t bytes_received{0};
    TimePoint last_activity;
    
    ConnectionInfo(SocketWrapper&& sock, const sockaddr_in& address) 
        : socket(std::move(sock)), addr(address), last_activity(Clock::now()) {}
};

template<Protocol P>
class NetworkServer {
private:
    int port_;
    int thread_id_;
    std::optional<int> buffer_size_;
    NetworkStats& stats_;
    std::atomic<bool>& stop_flag_;
    
public:
    NetworkServer(int port, int thread_id, std::optional<int> buffer_size,
                  NetworkStats& stats, std::atomic<bool>& stop_flag)
        : port_(port), thread_id_(thread_id), buffer_size_(buffer_size),
          stats_(stats), stop_flag_(stop_flag) {}
    
    void run_select() {
        auto listen_socket = create_listen_socket();
        listen_socket.set_nonblocking();
        
        fd_set read_fds, master_read_fds;
        FD_ZERO(&master_read_fds);
        FD_SET(listen_socket.get(), &master_read_fds);
        int max_fd = listen_socket.get();
        
        std::vector<std::unique_ptr<ConnectionInfo>> connections;
        std::vector<char> buffer(64 * KB);
        
        auto start_time = Clock::now();
        
        while (!stop_flag_.load()) {
            read_fds = master_read_fds;
            
            timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 100000; // 100ms
            
            int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, &timeout);
            if (ready < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("select failed: " + std::string(strerror(errno)));
            }
            
            // Handle new connections
            if (FD_ISSET(listen_socket.get(), &read_fds)) {
                handle_new_connections(listen_socket, master_read_fds, max_fd, connections);
            }
            
            // Handle client data
            handle_client_data(connections, read_fds, master_read_fds, buffer);
        }
        
        auto end_time = Clock::now();
        stats_.total_time.store(Duration(end_time - start_time).count());
    }
    
#ifdef __linux__
    void run_epoll() {
        auto listen_socket = create_listen_socket();
        listen_socket.set_nonblocking();
        
        int epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) {
            throw std::runtime_error("epoll_create1 failed: " + std::string(strerror(errno)));
        }
        
        struct epoll_event ev, events[MAX_EVENTS];
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = listen_socket.get();
        
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_socket.get(), &ev) < 0) {
            close(epoll_fd);
            throw std::runtime_error("epoll_ctl failed: " + std::string(strerror(errno)));
        }
        
        std::map<int, std::unique_ptr<ConnectionInfo>> connections;
        std::vector<char> buffer(64 * KB);
        
        auto start_time = Clock::now();
        
        while (!stop_flag_.load()) {
            int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 100);
            if (nfds < 0) {
                if (errno == EINTR) continue;
                break;
            }
            
            for (int i = 0; i < nfds; i++) {
                if (events[i].data.fd == listen_socket.get()) {
                    // Handle new connections
                    while (true) {
                        sockaddr_in client_addr;
                        socklen_t client_len = sizeof(client_addr);
                        int client_fd = accept(listen_socket.get(), 
                                             reinterpret_cast<sockaddr*>(&client_addr), &client_len);
                        
                        if (client_fd < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            continue;
                        }
                        
                        SocketWrapper client_socket(client_fd);
                        client_socket.set_nonblocking();
                        if (buffer_size_) client_socket.set_buffer_size(*buffer_size_);
                        if constexpr (P == Protocol::TCP) client_socket.set_tcp_nodelay();
                        
                        ev.events = EPOLLIN | EPOLLET;
                        ev.data.fd = client_fd;
                        
                        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == 0) {
                            connections[client_fd] = std::make_unique<ConnectionInfo>(
                                std::move(client_socket), client_addr);
                        }
                    }
                } else {
                    // Handle client data
                    int client_fd = events[i].data.fd;
                    auto it = connections.find(client_fd);
                    if (it == connections.end()) continue;
                    
                    bool should_close = false;
                    
                    while (true) {
                        ssize_t n = recv(client_fd, buffer.data(), buffer.size(), 0);
                        
                        if (n > 0) {
                            stats_.bytes_received.fetch_add(n, std::memory_order_relaxed);
                            stats_.messages_received.fetch_add(1, std::memory_order_relaxed);
                            
                            // Echo back
                            ssize_t sent = send(client_fd, buffer.data(), n, 0);
                            if (sent > 0) {
                                stats_.bytes_sent.fetch_add(sent, std::memory_order_relaxed);
                                stats_.messages_sent.fetch_add(1, std::memory_order_relaxed);
                            }
                            
                            it->second->last_activity = Clock::now();
                        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                            should_close = true;
                            break;
                        } else {
                            break;
                        }
                    }
                    
                    if (should_close) {
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                        connections.erase(it);
                    }
                }
            }
        }
        
        auto end_time = Clock::now();
        stats_.total_time.store(Duration(end_time - start_time).count());
        
        close(epoll_fd);
    }
#endif

#ifdef __APPLE__
    void run_kqueue() {
        auto listen_socket = create_listen_socket();
        listen_socket.set_nonblocking();
        
        int kq = kqueue();
        if (kq < 0) {
            throw std::runtime_error("kqueue failed: " + std::string(strerror(errno)));
        }
        
        struct kevent change_event, events[MAX_EVENTS];
        EV_SET(&change_event, listen_socket.get(), EVFILT_READ, EV_ADD, 0, 0, nullptr);
        
        if (kevent(kq, &change_event, 1, nullptr, 0, nullptr) < 0) {
            close(kq);
            throw std::runtime_error("kevent failed: " + std::string(strerror(errno)));
        }
        
        std::map<int, std::unique_ptr<ConnectionInfo>> connections;
        std::vector<char> buffer(64 * KB);
        
        auto start_time = Clock::now();
        
        while (!stop_flag_.load()) {
            struct timespec timeout = {0, 100000000}; // 100ms
            int nev = kevent(kq, nullptr, 0, events, MAX_EVENTS, &timeout);
            
            if (nev < 0) {
                if (errno == EINTR) continue;
                break;
            }
            
            for (int i = 0; i < nev; i++) {
                if (events[i].ident == static_cast<uintptr_t>(listen_socket.get())) {
                    // Handle new connections
                    while (true) {
                        sockaddr_in client_addr;
                        socklen_t client_len = sizeof(client_addr);
                        int client_fd = accept(listen_socket.get(), 
                                             reinterpret_cast<sockaddr*>(&client_addr), &client_len);
                        
                        if (client_fd < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                            continue;
                        }
                        
                        SocketWrapper client_socket(client_fd);
                        client_socket.set_nonblocking();
                        if (buffer_size_) client_socket.set_buffer_size(*buffer_size_);
                        if constexpr (P == Protocol::TCP) client_socket.set_tcp_nodelay();
                        
                        EV_SET(&change_event, client_fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
                        if (kevent(kq, &change_event, 1, nullptr, 0, nullptr) == 0) {
                            connections[client_fd] = std::make_unique<ConnectionInfo>(
                                std::move(client_socket), client_addr);
                        }
                    }
                } else {
                    // Handle client data
                    int client_fd = events[i].ident;
                    auto it = connections.find(client_fd);
                    if (it == connections.end()) continue;
                    
                    bool should_close = false;
                    
                    if (events[i].filter == EVFILT_READ) {
                        while (true) {
                            ssize_t n = recv(client_fd, buffer.data(), buffer.size(), 0);
                            
                            if (n > 0) {
                                stats_.bytes_received.fetch_add(n, std::memory_order_relaxed);
                                stats_.messages_received.fetch_add(1, std::memory_order_relaxed);
                                
                                // Echo back
                                ssize_t sent = send(client_fd, buffer.data(), n, 0);
                                if (sent > 0) {
                                    stats_.bytes_sent.fetch_add(sent, std::memory_order_relaxed);
                                    stats_.messages_sent.fetch_add(1, std::memory_order_relaxed);
                                }
                                
                                it->second->last_activity = Clock::now();
                            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                                should_close = true;
                                break;
                            } else {
                                break;
                            }
                        }
                    }
                    
                    if (should_close) {
                        EV_SET(&change_event, client_fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
                        kevent(kq, &change_event, 1, nullptr, 0, nullptr);
                        connections.erase(it);
                    }
                }
            }
        }
        
        auto end_time = Clock::now();
        stats_.total_time.store(Duration(end_time - start_time).count());
        
        close(kq);
    }
#endif

private:
    SocketWrapper create_listen_socket() {
        SocketWrapper socket;
        
        if constexpr (P == Protocol::TCP) {
            socket = SocketWrapper::create_tcp();
        } else {
            socket = SocketWrapper::create_udp();
        }
        
        socket.set_reuse_addr();
        if (buffer_size_) {
            socket.set_buffer_size(*buffer_size_);
        }
        
        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(port_ + thread_id_);
        
        if (bind(socket.get(), reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
            throw std::runtime_error("Bind failed: " + std::string(strerror(errno)));
        }
        
        if constexpr (P == Protocol::TCP) {
            if (listen(socket.get(), 128) < 0) {
                throw std::runtime_error("Listen failed: " + std::string(strerror(errno)));
            }
        }
        
        return socket;
    }
    
    void handle_new_connections(SocketWrapper& listen_socket, fd_set& master_fds, int& max_fd,
                               std::vector<std::unique_ptr<ConnectionInfo>>& connections) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(listen_socket.get(), 
                             reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        
        if (client_fd >= 0 && connections.size() < MAX_CONNECTIONS) {
            SocketWrapper client_socket(client_fd);
            client_socket.set_nonblocking();
            
            if (buffer_size_) {
                client_socket.set_buffer_size(*buffer_size_);
            }
            
            if constexpr (P == Protocol::TCP) {
                client_socket.set_tcp_nodelay();
            }
            
            FD_SET(client_fd, &master_fds);
            if (client_fd > max_fd) max_fd = client_fd;
            
            connections.push_back(std::make_unique<ConnectionInfo>(
                std::move(client_socket), client_addr));
        }
    }
    
    void handle_client_data(std::vector<std::unique_ptr<ConnectionInfo>>& connections,
                           fd_set& read_fds, fd_set& master_fds,
                           std::vector<char>& buffer) {
        for (auto it = connections.begin(); it != connections.end(); ) {
            int client_fd = (*it)->socket.get();
            
            if (FD_ISSET(client_fd, &read_fds)) {
                ssize_t n = recv(client_fd, buffer.data(), buffer.size(), 0);
                
                if (n > 0) {
                    stats_.bytes_received.fetch_add(n, std::memory_order_relaxed);
                    stats_.messages_received.fetch_add(1, std::memory_order_relaxed);
                    (*it)->bytes_received += n;
                    
                    // Echo back
                    ssize_t sent = send(client_fd, buffer.data(), n, 0);
                    if (sent > 0) {
                        stats_.bytes_sent.fetch_add(sent, std::memory_order_relaxed);
                        stats_.messages_sent.fetch_add(1, std::memory_order_relaxed);
                        (*it)->bytes_sent += sent;
                    }
                    
                    (*it)->last_activity = Clock::now();
                    ++it;
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    // Connection closed
                    FD_CLR(client_fd, &master_fds);
                    it = connections.erase(it);
                } else {
                    ++it;
                }
            } else {
                ++it;
            }
        }
    }
};

template<Protocol P>
class NetworkClient {
private:
    std::string server_addr_;
    int port_;
    size_t message_size_;
    int num_messages_;
    std::optional<int> buffer_size_;
    NetworkStats& stats_;
    std::atomic<bool>& stop_flag_;
    
public:
    NetworkClient(const std::string& server_addr, int port, size_t message_size,
                  int num_messages, std::optional<int> buffer_size,
                  NetworkStats& stats, std::atomic<bool>& stop_flag)
        : server_addr_(server_addr), port_(port), message_size_(message_size),
          num_messages_(num_messages), buffer_size_(buffer_size),
          stats_(stats), stop_flag_(stop_flag) {}
    
    void run() {
        SocketWrapper socket;
        
        if constexpr (P == Protocol::TCP) {
            socket = SocketWrapper::create_tcp();
        } else {
            socket = SocketWrapper::create_udp();
        }
        
        if (buffer_size_) {
            socket.set_buffer_size(*buffer_size_);
        }
        
        if constexpr (P == Protocol::TCP) {
            socket.set_tcp_nodelay();
        }
        
        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port_);
        if (inet_pton(AF_INET, server_addr_.c_str(), &server_addr.sin_addr) <= 0) {
            throw std::runtime_error("Invalid server address: " + server_addr_);
        }
        
        if constexpr (P == Protocol::TCP) {
            if (connect(socket.get(), reinterpret_cast<sockaddr*>(&server_addr), 
                       sizeof(server_addr)) < 0) {
                throw std::runtime_error("Connect failed: " + std::string(strerror(errno)));
            }
        }
        
        std::vector<char> send_buffer(message_size_);
        std::vector<char> recv_buffer(message_size_);
        
        // Initialize send buffer
        std::iota(send_buffer.begin(), send_buffer.end(), 0);
        
        auto start_time = Clock::now();
        
        for (int i = 0; i < num_messages_ && !stop_flag_.load(); i++) {
            auto send_time = Clock::now();
            
            // Send message
            ssize_t sent;
            if constexpr (P == Protocol::TCP) {
                sent = send(socket.get(), send_buffer.data(), message_size_, 0);
            } else {
                sent = sendto(socket.get(), send_buffer.data(), message_size_, 0,
                            reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
            }
            
            if (sent != static_cast<ssize_t>(message_size_)) {
                if (sent < 0) {
                    std::cerr << "Send error: " << strerror(errno) << std::endl;
                }
                continue;
            }
            
            stats_.bytes_sent.fetch_add(sent, std::memory_order_relaxed);
            stats_.messages_sent.fetch_add(1, std::memory_order_relaxed);
            
            // Receive echo
            if constexpr (P == Protocol::TCP) {
                size_t total_received = 0;
                while (total_received < message_size_) {
                    ssize_t n = recv(socket.get(), recv_buffer.data() + total_received,
                                   message_size_ - total_received, 0);
                    if (n <= 0) {
                        if (n < 0) {
                            std::cerr << "Recv error: " << strerror(errno) << std::endl;
                        }
                        goto done;
                    }
                    total_received += n;
                }
                
                auto recv_time = Clock::now();
                double latency = Duration(recv_time - send_time).count();
                
                stats_.bytes_received.fetch_add(total_received, std::memory_order_relaxed);
                stats_.messages_received.fetch_add(1, std::memory_order_relaxed);
                stats_.add_latency_sample(latency);
            } else {
                sockaddr_in from_addr;
                socklen_t from_len = sizeof(from_addr);
                ssize_t n = recvfrom(socket.get(), recv_buffer.data(), message_size_, 0,
                                   reinterpret_cast<sockaddr*>(&from_addr), &from_len);
                
                if (n > 0) {
                    auto recv_time = Clock::now();
                    double latency = Duration(recv_time - send_time).count();
                    
                    stats_.bytes_received.fetch_add(n, std::memory_order_relaxed);
                    stats_.messages_received.fetch_add(1, std::memory_order_relaxed);
                    stats_.add_latency_sample(latency);
                }
            }
        }
        
done:
        auto end_time = Clock::now();
        stats_.total_time.store(Duration(end_time - start_time).count());
    }
};

class NetworkBenchmarker {
private:
    std::string server_addr_;
    int port_;
    bool verbose_;
    
public:
    NetworkBenchmarker(const std::string& server_addr, int port, bool verbose = false)
        : server_addr_(server_addr), port_(port), verbose_(verbose) {}
    
    void run_throughput_test(Protocol protocol, size_t message_size, int duration, int num_threads) {
        std::cout << "\n=== Throughput Test (" 
                  << (protocol == Protocol::TCP ? "TCP" : "UDP") << ") ===\n";
        std::cout << "Message size: " << message_size << " bytes\n";
        std::cout << "Duration: " << duration << " seconds\n";
        std::cout << "Threads: " << num_threads << "\n";
        
        NetworkStats server_stats;
        NetworkStats client_stats;
        std::atomic<bool> stop_flag{false};
        
        // Start server
        auto server_future = std::async(std::launch::async, [&]() {
            try {
                if (protocol == Protocol::TCP) {
                    NetworkServer<Protocol::TCP> server(port_, 0, std::nullopt, 
                                                      server_stats, stop_flag);
                    server.run_select();
                } else {
                    NetworkServer<Protocol::UDP> server(port_, 0, std::nullopt, 
                                                      server_stats, stop_flag);
                    server.run_select();
                }
            } catch (const std::exception& e) {
                std::cerr << "Server error: " << e.what() << std::endl;
            }
        });
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Start client
        auto client_future = std::async(std::launch::async, [&]() {
            try {
                if (protocol == Protocol::TCP) {
                    NetworkClient<Protocol::TCP> client(server_addr_, port_, message_size,
                                                      duration * 1000, std::nullopt,
                                                      client_stats, stop_flag);
                    client.run();
                } else {
                    NetworkClient<Protocol::UDP> client(server_addr_, port_, message_size,
                                                      duration * 1000, std::nullopt,
                                                      client_stats, stop_flag);
                    client.run();
                }
            } catch (const std::exception& e) {
                std::cerr << "Client error: " << e.what() << std::endl;
            }
        });
        
        // Run for specified duration
        std::this_thread::sleep_for(std::chrono::seconds(duration));
        stop_flag.store(true);
        
        client_future.wait();
        server_future.wait();
        
        print_statistics(client_stats, "Client");
        print_statistics(server_stats, "Server");
    }
    
    void run_buffer_size_test(Protocol protocol) {
        std::cout << "\n=== Socket Buffer Size Optimization Test (" 
                  << (protocol == Protocol::TCP ? "TCP" : "UDP") << ") ===\n";
        
        std::vector<int> buffer_sizes = {8*KB, 16*KB, 32*KB, 64*KB, 128*KB, 256*KB, 512*KB, 1*MB};
        
        std::cout << std::left << std::setw(12) << "Buffer Size"
                  << std::setw(15) << "Throughput MB/s"
                  << std::setw(15) << "Messages/sec"
                  << std::setw(15) << "Avg Latency ms" << "\n";
        std::cout << std::string(57, '-') << "\n";
        
        for (size_t i = 0; i < buffer_sizes.size(); i++) {
            NetworkStats client_stats;
            std::atomic<bool> stop_flag{false};
            
            // Start server with buffer size
            auto server_future = std::async(std::launch::async, [&]() {
                NetworkStats server_stats;
                try {
                    if (protocol == Protocol::TCP) {
                        NetworkServer<Protocol::TCP> server(port_ + i, 0, buffer_sizes[i],
                                                          server_stats, stop_flag);
                        server.run_select();
                    } else {
                        NetworkServer<Protocol::UDP> server(port_ + i, 0, buffer_sizes[i],
                                                          server_stats, stop_flag);
                        server.run_select();
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Server error: " << e.what() << std::endl;
                }
            });
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            // Start client with buffer size
            auto client_future = std::async(std::launch::async, [&]() {
                try {
                    if (protocol == Protocol::TCP) {
                        NetworkClient<Protocol::TCP> client(server_addr_, port_ + i, 64*KB,
                                                          1000, buffer_sizes[i],
                                                          client_stats, stop_flag);
                        client.run();
                    } else {
                        NetworkClient<Protocol::UDP> client(server_addr_, port_ + i, 64*KB,
                                                          1000, buffer_sizes[i],
                                                          client_stats, stop_flag);
                        client.run();
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Client error: " << e.what() << std::endl;
                }
            });
            
            client_future.wait();
            stop_flag.store(true);
            server_future.wait();
            
            // Calculate results
            double throughput = 0;
            double msg_per_sec = 0;
            double avg_latency = 0;
            
            double total_time = client_stats.total_time.load();
            if (total_time > 0) {
                uint64_t total_bytes = client_stats.bytes_sent.load() + client_stats.bytes_received.load();
                uint64_t total_messages = client_stats.messages_sent.load() + client_stats.messages_received.load();
                
                throughput = (total_bytes / static_cast<double>(MB)) / total_time;
                msg_per_sec = total_messages / total_time;
            }
            
            uint64_t latency_count = client_stats.latency_count.load();
            if (latency_count > 0) {
                avg_latency = client_stats.total_latency.load() / latency_count * 1000;
            }
            
            std::string size_str;
            if (static_cast<size_t>(buffer_sizes[i]) >= MB) {
                size_str = std::to_string(buffer_sizes[i] / static_cast<int>(MB)) + "MB";
            } else {
                size_str = std::to_string(buffer_sizes[i] / static_cast<int>(KB)) + "KB";
            }
            
            std::cout << std::left << std::setw(12) << size_str
                      << std::setw(15) << std::fixed << std::setprecision(1) << throughput
                      << std::setw(15) << std::fixed << std::setprecision(0) << msg_per_sec
                      << std::setw(15) << std::fixed << std::setprecision(3) << avg_latency << "\n";
        }
    }
    
    void run_multiplexer_test() {
        std::cout << "\n=== I/O Multiplexer Comparison Test ===\n";
        std::cout << "Testing with message size: 1KB, duration: 5 seconds\n\n";
        
        struct MultiplexerTest {
            std::string name;
            std::function<void(NetworkServer<Protocol::TCP>&)> run_func;
            bool available;
        };
        
        std::vector<MultiplexerTest> multiplexers = {
            {"select", [](NetworkServer<Protocol::TCP>& s) { s.run_select(); }, true},
#ifdef __linux__
            {"epoll", [](NetworkServer<Protocol::TCP>& s) { s.run_epoll(); }, true},
#endif
#ifdef __APPLE__
            {"kqueue", [](NetworkServer<Protocol::TCP>& s) { s.run_kqueue(); }, true},
#endif
        };
        
        for (const auto& mux : multiplexers) {
            if (!mux.available) continue;
            
            std::cout << "\n" << mux.name << " performance:\n";
            std::cout << std::left << std::setw(15) << "Throughput MB/s"
                      << std::setw(15) << "Messages/sec"
                      << std::setw(15) << "Avg Latency ms" << "\n";
            std::cout << std::string(45, '-') << "\n";
            
            NetworkStats server_stats;
            NetworkStats client_stats;
            std::atomic<bool> stop_flag{false};
            
            // Start server with specific multiplexer
            auto server_future = std::async(std::launch::async, [&]() {
                try {
                    NetworkServer<Protocol::TCP> server(port_, 0, std::nullopt,
                                                      server_stats, stop_flag);
                    mux.run_func(server);
                } catch (const std::exception& e) {
                    std::cerr << "Server error: " << e.what() << std::endl;
                }
            });
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            
            // Start client
            auto client_future = std::async(std::launch::async, [&]() {
                try {
                    NetworkClient<Protocol::TCP> client(server_addr_, port_, 1024,
                                                      5000, std::nullopt,
                                                      client_stats, stop_flag);
                    client.run();
                } catch (const std::exception& e) {
                    std::cerr << "Client error: " << e.what() << std::endl;
                }
            });
            
            client_future.wait();
            stop_flag.store(true);
            server_future.wait();
            
            // Calculate and print results
            double throughput = 0;
            double msg_per_sec = 0;
            double avg_latency = 0;
            
            double total_time = client_stats.total_time.load();
            if (total_time > 0) {
                uint64_t total_bytes = client_stats.bytes_sent.load() + client_stats.bytes_received.load();
                uint64_t total_messages = client_stats.messages_sent.load() + client_stats.messages_received.load();
                
                throughput = (total_bytes / static_cast<double>(MB)) / total_time;
                msg_per_sec = total_messages / total_time;
            }
            
            uint64_t latency_count = client_stats.latency_count.load();
            if (latency_count > 0) {
                avg_latency = client_stats.total_latency.load() / latency_count * 1000;
            }
            
            std::cout << std::left << std::setw(15) << std::fixed << std::setprecision(1) << throughput
                      << std::setw(15) << std::fixed << std::setprecision(0) << msg_per_sec
                      << std::setw(15) << std::fixed << std::setprecision(3) << avg_latency << "\n";
        }
    }
    
    void run_all_tests() {
        std::cout << "\n### TCP Performance ###\n";
        run_throughput_test(Protocol::TCP, DEFAULT_MESSAGE_SIZE, 10, 1);
        run_buffer_size_test(Protocol::TCP);
        
        std::cout << "\n### UDP Performance ###\n";
        run_throughput_test(Protocol::UDP, DEFAULT_MESSAGE_SIZE, 10, 1);
        run_buffer_size_test(Protocol::UDP);
        
        run_multiplexer_test();
    }
    
private:
    void print_statistics(const NetworkStats& stats, const std::string& name) const {
        std::cout << "\n=== " << name << " Results ===\n";
        
        double total_time = stats.total_time.load();
        uint64_t messages_sent = stats.messages_sent.load();
        uint64_t messages_received = stats.messages_received.load();
        uint64_t bytes_sent = stats.bytes_sent.load();
        uint64_t bytes_received = stats.bytes_received.load();
        
        std::cout << "Duration: " << std::fixed << std::setprecision(2) << total_time << " seconds\n";
        std::cout << "Messages sent: " << messages_sent << "\n";
        std::cout << "Messages received: " << messages_received << "\n";
        std::cout << "Bytes sent: " << std::fixed << std::setprecision(2) 
                  << bytes_sent / static_cast<double>(MB) << " MB\n";
        std::cout << "Bytes received: " << std::fixed << std::setprecision(2) 
                  << bytes_received / static_cast<double>(MB) << " MB\n";
        
        if (total_time > 0) {
            double throughput_mbps = (bytes_sent + bytes_received) / static_cast<double>(MB) / total_time;
            double msg_per_sec = (messages_sent + messages_received) / total_time;
            
            std::cout << "Throughput: " << std::fixed << std::setprecision(2) << throughput_mbps << " MB/s\n";
            std::cout << "Messages/sec: " << std::fixed << std::setprecision(0) << msg_per_sec << "\n";
        }
        
        uint64_t latency_count = stats.latency_count.load();
        if (latency_count > 0) {
            double avg_latency = stats.total_latency.load() / latency_count;
            double min_latency = stats.min_latency.load();
            double max_latency = stats.max_latency.load();
            
            std::cout << "\nLatency Statistics:\n";
            std::cout << "Average: " << std::fixed << std::setprecision(3) << avg_latency * 1000 << " ms\n";
            std::cout << "Min: " << std::fixed << std::setprecision(3) << min_latency * 1000 << " ms\n";
            std::cout << "Max: " << std::fixed << std::setprecision(3) << max_latency * 1000 << " ms\n";
            
            if (verbose_) {
                auto percentiles = stats.get_percentiles();
                if (!percentiles.empty()) {
                    std::cout << "\nLatency Percentiles:\n";
                    std::vector<std::string> labels = {"50th", "75th", "90th", "95th", "99th", "99.9th"};
                    for (size_t i = 0; i < percentiles.size() && i < labels.size(); ++i) {
                        std::cout << labels[i] << " percentile: " << std::fixed 
                                  << std::setprecision(3) << percentiles[i] * 1000 << " ms\n";
                    }
                }
            }
        }
    }
};

void print_usage(const std::string& prog_name) {
    std::cout << "Usage: " << prog_name << " [options]\n";
    std::cout << "Options:\n";
    std::cout << "  -a <address>   Server address (default: 127.0.0.1)\n";
    std::cout << "  -p <port>      Port number (default: " << DEFAULT_PORT << ")\n";
    std::cout << "  -P <protocol>  Protocol: tcp or udp (default: tcp)\n";
    std::cout << "  -t <type>      Test type: throughput, latency, buffer, multiplexer, all\n";
    std::cout << "  -s <size>      Message size in bytes (default: " << DEFAULT_MESSAGE_SIZE << ")\n";
    std::cout << "  -n <count>     Number of messages (default: " << DEFAULT_NUM_MESSAGES << ")\n";
    std::cout << "  -d <duration>  Test duration in seconds (default: 10)\n";
    std::cout << "  -T <threads>   Number of threads (default: 1)\n";
    std::cout << "  -v             Verbose output with percentiles\n";
    std::cout << "  -h             Show this help message\n";
    std::cout << "\nAnalyzes network I/O performance characteristics:\n";
    std::cout << "  - TCP vs UDP throughput and latency\n";
    std::cout << "  - Socket buffer size optimization\n";
    std::cout << "  - I/O multiplexer comparison (select/epoll/kqueue)\n";
    std::cout << "  - Connection scaling analysis\n";
    std::cout << "  - Modern C++ async I/O patterns\n";
}

} // namespace NetworkIO

int main(int argc, char* argv[]) {
    using namespace NetworkIO;
    
    std::string server_addr = "127.0.0.1";
    int port = DEFAULT_PORT;
    Protocol protocol = Protocol::TCP;
    TestType test_type = TestType::All;
    size_t message_size = DEFAULT_MESSAGE_SIZE;
    int num_messages = DEFAULT_NUM_MESSAGES;
    int duration = 10;
    int num_threads = 1;
    bool verbose = false;
    
    // Ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);
    
    int opt;
    while ((opt = getopt(argc, argv, "a:p:P:t:s:n:d:T:vh")) != -1) {
        switch (opt) {
            case 'a':
                server_addr = optarg;
                break;
            case 'p':
                port = std::stoi(optarg);
                if (port <= 0 || port > 65535) {
                    std::cerr << "Invalid port: " << optarg << std::endl;
                    return 1;
                }
                break;
            case 'P':
                if (std::string_view(optarg) == "tcp") protocol = Protocol::TCP;
                else if (std::string_view(optarg) == "udp") protocol = Protocol::UDP;
                else {
                    std::cerr << "Invalid protocol: " << optarg << std::endl;
                    return 1;
                }
                break;
            case 't':
                if (std::string_view(optarg) == "throughput") test_type = TestType::Throughput;
                else if (std::string_view(optarg) == "latency") test_type = TestType::Latency;
                else if (std::string_view(optarg) == "buffer") test_type = TestType::BufferSize;
                else if (std::string_view(optarg) == "multiplexer") test_type = TestType::Multiplexer;
                else if (std::string_view(optarg) == "all") test_type = TestType::All;
                else {
                    std::cerr << "Invalid test type: " << optarg << std::endl;
                    return 1;
                }
                break;
            case 's':
                message_size = std::stoi(optarg);
                if (message_size <= 0) {
                    std::cerr << "Invalid message size: " << optarg << std::endl;
                    return 1;
                }
                break;
            case 'n':
                num_messages = std::stoi(optarg);
                if (num_messages <= 0) {
                    std::cerr << "Invalid message count: " << optarg << std::endl;
                    return 1;
                }
                break;
            case 'd':
                duration = std::stoi(optarg);
                if (duration <= 0) {
                    std::cerr << "Invalid duration: " << optarg << std::endl;
                    return 1;
                }
                break;
            case 'T':
                num_threads = std::stoi(optarg);
                if (num_threads <= 0) {
                    std::cerr << "Invalid thread count: " << optarg << std::endl;
                    return 1;
                }
                break;
            case 'v':
                verbose = true;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }
    
    std::cout << "Network I/O Benchmarker (C++)\n";
    std::cout << "==============================\n";
    std::cout << "Server address: " << server_addr << "\n";
    std::cout << "Port: " << port << "\n";
    std::cout << "Protocol: " << (protocol == Protocol::TCP ? "TCP" : "UDP") << "\n";
    std::cout << "Verbose: " << (verbose ? "enabled" : "disabled") << "\n";
    
    try {
        NetworkBenchmarker benchmarker(server_addr, port, verbose);
        
        switch (test_type) {
            case TestType::Throughput:
                benchmarker.run_throughput_test(protocol, message_size, duration, num_threads);
                break;
            case TestType::Latency:
                // Use smaller messages for latency test
                benchmarker.run_throughput_test(protocol, 64, duration, 1);
                break;
            case TestType::BufferSize:
                benchmarker.run_buffer_size_test(protocol);
                break;
            case TestType::Multiplexer:
                if (protocol == Protocol::TCP) {
                    benchmarker.run_multiplexer_test();
                } else {
                    std::cout << "Multiplexer test only available for TCP\n";
                }
                break;
            case TestType::All:
                benchmarker.run_all_tests();
                break;
            default:
                break;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}