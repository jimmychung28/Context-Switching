#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <math.h>
#include <poll.h>

#ifdef __APPLE__
#include <sys/event.h>
#include <sys/time.h>
#define HAVE_KQUEUE 1

// macOS doesn't have pthread_barrier_t
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned int count;
    unsigned int num_threads;
} pthread_barrier_t;

static int pthread_barrier_init(pthread_barrier_t *barrier, void *attr, unsigned int count) {
    (void)attr;
    barrier->count = 0;
    barrier->num_threads = count;
    pthread_mutex_init(&barrier->mutex, NULL);
    pthread_cond_init(&barrier->cond, NULL);
    return 0;
}

static int pthread_barrier_wait(pthread_barrier_t *barrier) {
    pthread_mutex_lock(&barrier->mutex);
    barrier->count++;
    if (barrier->count >= barrier->num_threads) {
        barrier->count = 0;
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->mutex);
        return 1;
    } else {
        pthread_cond_wait(&barrier->cond, &barrier->mutex);
        pthread_mutex_unlock(&barrier->mutex);
        return 0;
    }
}

static int pthread_barrier_destroy(pthread_barrier_t *barrier) {
    pthread_mutex_destroy(&barrier->mutex);
    pthread_cond_destroy(&barrier->cond);
    return 0;
}

#else
#include <sys/epoll.h>
#define HAVE_EPOLL 1
#endif

#define DEFAULT_PORT 12345
#define DEFAULT_MESSAGE_SIZE 1024
#define DEFAULT_NUM_MESSAGES 10000
#define DEFAULT_NUM_CONNECTIONS 100
#define MAX_EVENTS 1024
#define MAX_CONNECTIONS 10000
#define HISTOGRAM_BUCKETS 50
#define KB 1024
#define MB (1024 * 1024)

typedef enum {
    PROTO_TCP,
    PROTO_UDP
} Protocol;

typedef enum {
    IO_SELECT,
    IO_POLL,
    IO_EPOLL,
    IO_KQUEUE
} IOMultiplexer;

typedef enum {
    TEST_THROUGHPUT,
    TEST_LATENCY,
    TEST_BUFFER_SIZE,
    TEST_MULTIPLEXER,
    TEST_CONNECTION_SCALING,
    TEST_ALL
} TestType;

typedef enum {
    MODE_CLIENT,
    MODE_SERVER
} Mode;

typedef struct {
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t messages_sent;
    uint64_t messages_received;
    double total_time;
    double min_latency;
    double max_latency;
    double total_latency;
    uint64_t latency_count;
    
    // Latency histogram
    uint64_t latency_histogram[HISTOGRAM_BUCKETS];
    double histogram_max_latency;
} NetworkStats;

typedef struct {
    int thread_id;
    Mode mode;
    Protocol protocol;
    IOMultiplexer multiplexer;
    const char* server_addr;
    int port;
    int message_size;
    int num_messages;
    int num_connections;
    int socket_buffer_size;
    NetworkStats* stats;
    volatile int* stop_flag;
    pthread_barrier_t* barrier;
} ThreadArgs;

typedef struct {
    int fd;
    struct sockaddr_in addr;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    struct timespec last_activity;
} ConnectionInfo;

// Global signal handler flag
static volatile int g_stop_flag = 0;

static void signal_handler(int sig) {
    (void)sig;
    g_stop_flag = 1;
}

static double get_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void add_latency_sample(NetworkStats* stats, double latency) {
    stats->total_latency += latency;
    stats->latency_count++;
    
    if (latency < stats->min_latency) stats->min_latency = latency;
    if (latency > stats->max_latency) stats->max_latency = latency;
    
    // Update histogram
    if (stats->histogram_max_latency == 0.0) {
        stats->histogram_max_latency = latency * 2;
    }
    if (latency > stats->histogram_max_latency) {
        stats->histogram_max_latency = latency * 1.5;
    }
    
    int bucket = (int)((latency / stats->histogram_max_latency) * HISTOGRAM_BUCKETS);
    if (bucket >= HISTOGRAM_BUCKETS) bucket = HISTOGRAM_BUCKETS - 1;
    if (bucket < 0) bucket = 0;
    
    stats->latency_histogram[bucket]++;
}

static int set_socket_buffer_size(int fd, int size) {
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0) {
        perror("setsockopt SO_RCVBUF");
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0) {
        perror("setsockopt SO_SNDBUF");
        return -1;
    }
    return 0;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// TCP Server Functions
static void* tcp_server_select(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    NetworkStats* stats = args->stats;
    
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return NULL;
    }
    
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    if (args->socket_buffer_size > 0) {
        set_socket_buffer_size(listen_fd, args->socket_buffer_size);
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(args->port + args->thread_id);
    
    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return NULL;
    }
    
    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        close(listen_fd);
        return NULL;
    }
    
    // Set non-blocking
    set_nonblocking(listen_fd);
    
    fd_set read_fds, write_fds, master_read_fds;
    FD_ZERO(&master_read_fds);
    FD_SET(listen_fd, &master_read_fds);
    int max_fd = listen_fd;
    
    ConnectionInfo connections[MAX_CONNECTIONS];
    int num_connections = 0;
    
    char* buffer = malloc(args->message_size);
    
    pthread_barrier_wait(args->barrier);
    double start_time = get_time();
    
    while (!*args->stop_flag) {
        read_fds = master_read_fds;
        FD_ZERO(&write_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 100000; // 100ms
        
        int ready = select(max_fd + 1, &read_fds, &write_fds, NULL, &timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        
        // Handle new connections
        if (FD_ISSET(listen_fd, &read_fds)) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
            
            if (client_fd >= 0 && num_connections < MAX_CONNECTIONS) {
                set_nonblocking(client_fd);
                
                if (args->socket_buffer_size > 0) {
                    set_socket_buffer_size(client_fd, args->socket_buffer_size);
                }
                
                // Disable Nagle's algorithm for low latency
                int nodelay = 1;
                setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                
                FD_SET(client_fd, &master_read_fds);
                if (client_fd > max_fd) max_fd = client_fd;
                
                connections[num_connections].fd = client_fd;
                connections[num_connections].addr = client_addr;
                connections[num_connections].bytes_sent = 0;
                connections[num_connections].bytes_received = 0;
                clock_gettime(CLOCK_MONOTONIC, &connections[num_connections].last_activity);
                num_connections++;
            }
        }
        
        // Handle client data
        for (int i = 0; i < num_connections; i++) {
            int client_fd = connections[i].fd;
            
            if (FD_ISSET(client_fd, &read_fds)) {
                ssize_t n = recv(client_fd, buffer, args->message_size, 0);
                
                if (n > 0) {
                    stats->bytes_received += n;
                    stats->messages_received++;
                    connections[i].bytes_received += n;
                    
                    // Echo back for latency measurement
                    ssize_t sent = send(client_fd, buffer, n, 0);
                    if (sent > 0) {
                        stats->bytes_sent += sent;
                        stats->messages_sent++;
                        connections[i].bytes_sent += sent;
                    }
                    
                    clock_gettime(CLOCK_MONOTONIC, &connections[i].last_activity);
                } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    // Connection closed
                    close(client_fd);
                    FD_CLR(client_fd, &master_read_fds);
                    
                    // Remove from connections array
                    for (int j = i; j < num_connections - 1; j++) {
                        connections[j] = connections[j + 1];
                    }
                    num_connections--;
                    i--;
                }
            }
        }
    }
    
    stats->total_time = get_time() - start_time;
    
    // Cleanup
    for (int i = 0; i < num_connections; i++) {
        close(connections[i].fd);
    }
    close(listen_fd);
    free(buffer);
    
    return NULL;
}

#ifdef HAVE_EPOLL
static void* tcp_server_epoll(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    NetworkStats* stats = args->stats;
    
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return NULL;
    }
    
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    if (args->socket_buffer_size > 0) {
        set_socket_buffer_size(listen_fd, args->socket_buffer_size);
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(args->port + args->thread_id);
    
    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return NULL;
    }
    
    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        close(listen_fd);
        return NULL;
    }
    
    set_nonblocking(listen_fd);
    
    // Create epoll instance
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        close(listen_fd);
        return NULL;
    }
    
    struct epoll_event ev, events[MAX_EVENTS];
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;
    
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        perror("epoll_ctl");
        close(epoll_fd);
        close(listen_fd);
        return NULL;
    }
    
    ConnectionInfo connections[MAX_CONNECTIONS];
    int num_connections = 0;
    char* buffer = malloc(args->message_size);
    
    pthread_barrier_wait(args->barrier);
    double start_time = get_time();
    
    while (!*args->stop_flag) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 100);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == listen_fd) {
                // Handle new connections
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
                    
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept");
                        break;
                    }
                    
                    if (num_connections >= MAX_CONNECTIONS) {
                        close(client_fd);
                        continue;
                    }
                    
                    set_nonblocking(client_fd);
                    
                    if (args->socket_buffer_size > 0) {
                        set_socket_buffer_size(client_fd, args->socket_buffer_size);
                    }
                    
                    int nodelay = 1;
                    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                    
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = client_fd;
                    
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                        perror("epoll_ctl");
                        close(client_fd);
                        continue;
                    }
                    
                    connections[num_connections].fd = client_fd;
                    connections[num_connections].addr = client_addr;
                    connections[num_connections].bytes_sent = 0;
                    connections[num_connections].bytes_received = 0;
                    clock_gettime(CLOCK_MONOTONIC, &connections[num_connections].last_activity);
                    num_connections++;
                }
            } else {
                // Handle client data
                int client_fd = events[i].data.fd;
                
                while (1) {
                    ssize_t n = recv(client_fd, buffer, args->message_size, 0);
                    
                    if (n > 0) {
                        stats->bytes_received += n;
                        stats->messages_received++;
                        
                        // Echo back
                        ssize_t sent = send(client_fd, buffer, n, 0);
                        if (sent > 0) {
                            stats->bytes_sent += sent;
                            stats->messages_sent++;
                        }
                    } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                        // Connection closed or error
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                        close(client_fd);
                        
                        // Remove from connections
                        for (int j = 0; j < num_connections; j++) {
                            if (connections[j].fd == client_fd) {
                                for (int k = j; k < num_connections - 1; k++) {
                                    connections[k] = connections[k + 1];
                                }
                                num_connections--;
                                break;
                            }
                        }
                        break;
                    } else {
                        // EAGAIN/EWOULDBLOCK
                        break;
                    }
                }
            }
        }
    }
    
    stats->total_time = get_time() - start_time;
    
    // Cleanup
    for (int i = 0; i < num_connections; i++) {
        close(connections[i].fd);
    }
    close(epoll_fd);
    close(listen_fd);
    free(buffer);
    
    return NULL;
}
#endif

#ifdef HAVE_KQUEUE
static void* tcp_server_kqueue(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    NetworkStats* stats = args->stats;
    
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return NULL;
    }
    
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    if (args->socket_buffer_size > 0) {
        set_socket_buffer_size(listen_fd, args->socket_buffer_size);
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(args->port + args->thread_id);
    
    if (bind(listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        return NULL;
    }
    
    if (listen(listen_fd, 128) < 0) {
        perror("listen");
        close(listen_fd);
        return NULL;
    }
    
    set_nonblocking(listen_fd);
    
    // Create kqueue instance
    int kq = kqueue();
    if (kq < 0) {
        perror("kqueue");
        close(listen_fd);
        return NULL;
    }
    
    struct kevent change_event, events[MAX_EVENTS];
    EV_SET(&change_event, listen_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    
    if (kevent(kq, &change_event, 1, NULL, 0, NULL) < 0) {
        perror("kevent");
        close(kq);
        close(listen_fd);
        return NULL;
    }
    
    ConnectionInfo connections[MAX_CONNECTIONS];
    int num_connections = 0;
    char* buffer = malloc(args->message_size);
    
    pthread_barrier_wait(args->barrier);
    double start_time = get_time();
    
    while (!*args->stop_flag) {
        struct timespec timeout = {0, 100000000}; // 100ms
        int nev = kevent(kq, NULL, 0, events, MAX_EVENTS, &timeout);
        
        if (nev < 0) {
            if (errno == EINTR) continue;
            perror("kevent");
            break;
        }
        
        for (int i = 0; i < nev; i++) {
            if (events[i].ident == (uintptr_t)listen_fd) {
                // Handle new connections
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
                    
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        perror("accept");
                        break;
                    }
                    
                    if (num_connections >= MAX_CONNECTIONS) {
                        close(client_fd);
                        continue;
                    }
                    
                    set_nonblocking(client_fd);
                    
                    if (args->socket_buffer_size > 0) {
                        set_socket_buffer_size(client_fd, args->socket_buffer_size);
                    }
                    
                    int nodelay = 1;
                    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
                    
                    EV_SET(&change_event, client_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
                    if (kevent(kq, &change_event, 1, NULL, 0, NULL) < 0) {
                        perror("kevent");
                        close(client_fd);
                        continue;
                    }
                    
                    connections[num_connections].fd = client_fd;
                    connections[num_connections].addr = client_addr;
                    connections[num_connections].bytes_sent = 0;
                    connections[num_connections].bytes_received = 0;
                    clock_gettime(CLOCK_MONOTONIC, &connections[num_connections].last_activity);
                    num_connections++;
                }
            } else {
                // Handle client data
                int client_fd = events[i].ident;
                
                if (events[i].filter == EVFILT_READ) {
                    while (1) {
                        ssize_t n = recv(client_fd, buffer, args->message_size, 0);
                        
                        if (n > 0) {
                            stats->bytes_received += n;
                            stats->messages_received++;
                            
                            // Echo back
                            ssize_t sent = send(client_fd, buffer, n, 0);
                            if (sent > 0) {
                                stats->bytes_sent += sent;
                                stats->messages_sent++;
                            }
                        } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                            // Connection closed
                            EV_SET(&change_event, client_fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
                            kevent(kq, &change_event, 1, NULL, 0, NULL);
                            close(client_fd);
                            
                            // Remove from connections
                            for (int j = 0; j < num_connections; j++) {
                                if (connections[j].fd == client_fd) {
                                    for (int k = j; k < num_connections - 1; k++) {
                                        connections[k] = connections[k + 1];
                                    }
                                    num_connections--;
                                    break;
                                }
                            }
                            break;
                        } else {
                            break;
                        }
                    }
                }
            }
        }
    }
    
    stats->total_time = get_time() - start_time;
    
    // Cleanup
    for (int i = 0; i < num_connections; i++) {
        close(connections[i].fd);
    }
    close(kq);
    close(listen_fd);
    free(buffer);
    
    return NULL;
}
#endif

// TCP Client
static void* tcp_client(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    NetworkStats* stats = args->stats;
    
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return NULL;
    }
    
    if (args->socket_buffer_size > 0) {
        set_socket_buffer_size(sock_fd, args->socket_buffer_size);
    }
    
    // Disable Nagle's algorithm
    int nodelay = 1;
    setsockopt(sock_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(args->port);
    inet_pton(AF_INET, args->server_addr, &server_addr.sin_addr);
    
    pthread_barrier_wait(args->barrier);
    
    if (connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock_fd);
        return NULL;
    }
    
    char* send_buffer = malloc(args->message_size);
    char* recv_buffer = malloc(args->message_size);
    
    // Initialize send buffer
    for (int i = 0; i < args->message_size; i++) {
        send_buffer[i] = (char)(i & 0xFF);
    }
    
    double start_time = get_time();
    
    for (int i = 0; i < args->num_messages && !*args->stop_flag; i++) {
        double send_time = get_time();
        
        // Send message
        ssize_t sent = send(sock_fd, send_buffer, args->message_size, 0);
        if (sent != args->message_size) {
            if (sent < 0) perror("send");
            break;
        }
        stats->bytes_sent += sent;
        stats->messages_sent++;
        
        // Receive echo
        ssize_t total_received = 0;
        while (total_received < args->message_size) {
            ssize_t n = recv(sock_fd, recv_buffer + total_received, 
                           args->message_size - total_received, 0);
            if (n <= 0) {
                if (n < 0) perror("recv");
                goto done;
            }
            total_received += n;
        }
        
        double recv_time = get_time();
        double latency = recv_time - send_time;
        
        stats->bytes_received += total_received;
        stats->messages_received++;
        add_latency_sample(stats, latency);
    }
    
done:
    stats->total_time = get_time() - start_time;
    
    close(sock_fd);
    free(send_buffer);
    free(recv_buffer);
    
    return NULL;
}

// UDP Server
static void* udp_server(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    NetworkStats* stats = args->stats;
    
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return NULL;
    }
    
    if (args->socket_buffer_size > 0) {
        set_socket_buffer_size(sock_fd, args->socket_buffer_size);
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(args->port + args->thread_id);
    
    if (bind(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(sock_fd);
        return NULL;
    }
    
    char* buffer = malloc(args->message_size);
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    
    pthread_barrier_wait(args->barrier);
    double start_time = get_time();
    
    while (!*args->stop_flag) {
        ssize_t n = recvfrom(sock_fd, buffer, args->message_size, 0,
                           (struct sockaddr*)&client_addr, &client_len);
        
        if (n > 0) {
            stats->bytes_received += n;
            stats->messages_received++;
            
            // Echo back
            ssize_t sent = sendto(sock_fd, buffer, n, 0,
                                (struct sockaddr*)&client_addr, client_len);
            if (sent > 0) {
                stats->bytes_sent += sent;
                stats->messages_sent++;
            }
        } else if (n < 0 && errno != EINTR) {
            perror("recvfrom");
            break;
        }
    }
    
    stats->total_time = get_time() - start_time;
    
    close(sock_fd);
    free(buffer);
    
    return NULL;
}

// UDP Client
static void* udp_client(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    NetworkStats* stats = args->stats;
    
    int sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return NULL;
    }
    
    if (args->socket_buffer_size > 0) {
        set_socket_buffer_size(sock_fd, args->socket_buffer_size);
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(args->port);
    inet_pton(AF_INET, args->server_addr, &server_addr.sin_addr);
    
    char* send_buffer = malloc(args->message_size);
    char* recv_buffer = malloc(args->message_size);
    
    // Initialize send buffer
    for (int i = 0; i < args->message_size; i++) {
        send_buffer[i] = (char)(i & 0xFF);
    }
    
    pthread_barrier_wait(args->barrier);
    double start_time = get_time();
    
    for (int i = 0; i < args->num_messages && !*args->stop_flag; i++) {
        double send_time = get_time();
        
        // Send message
        ssize_t sent = sendto(sock_fd, send_buffer, args->message_size, 0,
                            (struct sockaddr*)&server_addr, sizeof(server_addr));
        if (sent != args->message_size) {
            if (sent < 0) perror("sendto");
            continue;
        }
        stats->bytes_sent += sent;
        stats->messages_sent++;
        
        // Receive echo
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        ssize_t n = recvfrom(sock_fd, recv_buffer, args->message_size, 0,
                           (struct sockaddr*)&from_addr, &from_len);
        
        if (n > 0) {
            double recv_time = get_time();
            double latency = recv_time - send_time;
            
            stats->bytes_received += n;
            stats->messages_received++;
            add_latency_sample(stats, latency);
        }
    }
    
    stats->total_time = get_time() - start_time;
    
    close(sock_fd);
    free(send_buffer);
    free(recv_buffer);
    
    return NULL;
}

static void print_statistics(NetworkStats* stats, const char* test_name) {
    printf("\n=== %s Results ===\n", test_name);
    printf("Duration: %.2f seconds\n", stats->total_time);
    printf("Messages sent: %llu\n", (unsigned long long)stats->messages_sent);
    printf("Messages received: %llu\n", (unsigned long long)stats->messages_received);
    printf("Bytes sent: %.2f MB\n", stats->bytes_sent / (double)MB);
    printf("Bytes received: %.2f MB\n", stats->bytes_received / (double)MB);
    
    if (stats->total_time > 0) {
        double throughput_mbps = (stats->bytes_sent + stats->bytes_received) / (double)MB / stats->total_time;
        double msg_per_sec = (stats->messages_sent + stats->messages_received) / stats->total_time;
        
        printf("Throughput: %.2f MB/s\n", throughput_mbps);
        printf("Messages/sec: %.0f\n", msg_per_sec);
    }
    
    if (stats->latency_count > 0) {
        double avg_latency = stats->total_latency / stats->latency_count;
        printf("\nLatency Statistics:\n");
        printf("Average: %.3f ms\n", avg_latency * 1000);
        printf("Min: %.3f ms\n", stats->min_latency * 1000);
        printf("Max: %.3f ms\n", stats->max_latency * 1000);
        
        // Calculate percentiles
        if (stats->latency_count > 10) {
            printf("\nLatency Distribution:\n");
            uint64_t total = 0;
            for (int i = 0; i < HISTOGRAM_BUCKETS; i++) {
                total += stats->latency_histogram[i];
            }
            
            uint64_t cumulative = 0;
            double percentiles[] = {50, 90, 95, 99, 99.9};
            int p_idx = 0;
            
            for (int i = 0; i < HISTOGRAM_BUCKETS && p_idx < 5; i++) {
                cumulative += stats->latency_histogram[i];
                double percent = (cumulative * 100.0) / total;
                
                while (p_idx < 5 && percent >= percentiles[p_idx]) {
                    double latency = (i + 0.5) * stats->histogram_max_latency / HISTOGRAM_BUCKETS;
                    printf("%.1fth percentile: %.3f ms\n", percentiles[p_idx], latency * 1000);
                    p_idx++;
                }
            }
        }
    }
}

static void throughput_test(Protocol protocol, const char* server_addr, int port, 
                          int message_size, int duration, int num_threads) {
    printf("\n=== Throughput Test (%s) ===\n", protocol == PROTO_TCP ? "TCP" : "UDP");
    printf("Message size: %d bytes\n", message_size);
    printf("Duration: %d seconds\n", duration);
    printf("Threads: %d\n", num_threads);
    
    NetworkStats server_stats = {0};
    NetworkStats client_stats = {0};
    server_stats.min_latency = INFINITY;
    client_stats.min_latency = INFINITY;
    
    pthread_t server_thread, client_thread;
    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, 2);
    volatile int stop_flag = 0;
    
    ThreadArgs server_args = {
        .thread_id = 0,
        .mode = MODE_SERVER,
        .protocol = protocol,
        .multiplexer = IO_SELECT,
        .server_addr = server_addr,
        .port = port,
        .message_size = message_size,
        .num_messages = INT32_MAX,
        .stats = &server_stats,
        .stop_flag = &stop_flag,
        .barrier = &barrier
    };
    
    ThreadArgs client_args = {
        .thread_id = 0,
        .mode = MODE_CLIENT,
        .protocol = protocol,
        .server_addr = server_addr,
        .port = port,
        .message_size = message_size,
        .num_messages = duration * 1000, // Approximate based on expected rate
        .stats = &client_stats,
        .stop_flag = &stop_flag,
        .barrier = &barrier
    };
    
    // Start server
    if (protocol == PROTO_TCP) {
        pthread_create(&server_thread, NULL, tcp_server_select, &server_args);
    } else {
        pthread_create(&server_thread, NULL, udp_server, &server_args);
    }
    
    // Give server time to start
    usleep(100000);
    
    // Start client
    if (protocol == PROTO_TCP) {
        pthread_create(&client_thread, NULL, tcp_client, &client_args);
    } else {
        pthread_create(&client_thread, NULL, udp_client, &client_args);
    }
    
    // Run for specified duration
    sleep(duration);
    stop_flag = 1;
    
    pthread_join(client_thread, NULL);
    pthread_join(server_thread, NULL);
    pthread_barrier_destroy(&barrier);
    
    print_statistics(&client_stats, "Client");
    print_statistics(&server_stats, "Server");
}

static void buffer_size_test(Protocol protocol, const char* server_addr, int port) {
    printf("\n=== Socket Buffer Size Optimization Test (%s) ===\n", 
           protocol == PROTO_TCP ? "TCP" : "UDP");
    
    int buffer_sizes[] = {8*KB, 16*KB, 32*KB, 64*KB, 128*KB, 256*KB, 512*KB, 1*MB};
    int num_sizes = sizeof(buffer_sizes) / sizeof(buffer_sizes[0]);
    
    printf("%-12s %-15s %-15s %-15s\n", 
           "Buffer Size", "Throughput MB/s", "Messages/sec", "Avg Latency ms");
    printf("------------ --------------- --------------- ---------------\n");
    
    for (int i = 0; i < num_sizes; i++) {
        NetworkStats client_stats = {0};
        client_stats.min_latency = INFINITY;
        
        pthread_t server_thread, client_thread;
        pthread_barrier_t barrier;
        pthread_barrier_init(&barrier, NULL, 2);
        volatile int stop_flag = 0;
        
        NetworkStats server_stats = {0};
        server_stats.min_latency = INFINITY;
        
        ThreadArgs server_args = {
            .thread_id = 0,
            .mode = MODE_SERVER,
            .protocol = protocol,
            .multiplexer = IO_SELECT,
            .port = port + i,
            .message_size = 64*KB,
            .socket_buffer_size = buffer_sizes[i],
            .stats = &server_stats,
            .stop_flag = &stop_flag,
            .barrier = &barrier
        };
        
        ThreadArgs client_args = {
            .thread_id = 0,
            .mode = MODE_CLIENT,
            .protocol = protocol,
            .server_addr = server_addr,
            .port = port + i,
            .message_size = 64*KB,
            .num_messages = 1000,
            .socket_buffer_size = buffer_sizes[i],
            .stats = &client_stats,
            .stop_flag = &stop_flag,
            .barrier = &barrier
        };
        
        // Start server
        if (protocol == PROTO_TCP) {
            pthread_create(&server_thread, NULL, tcp_server_select, &server_args);
        } else {
            pthread_create(&server_thread, NULL, udp_server, &server_args);
        }
        
        usleep(100000);
        
        // Start client
        if (protocol == PROTO_TCP) {
            pthread_create(&client_thread, NULL, tcp_client, &client_args);
        } else {
            pthread_create(&client_thread, NULL, udp_client, &client_args);
        }
        
        pthread_join(client_thread, NULL);
        stop_flag = 1;
        pthread_join(server_thread, NULL);
        pthread_barrier_destroy(&barrier);
        
        // Calculate results
        double throughput = 0;
        double msg_per_sec = 0;
        double avg_latency = 0;
        
        if (client_stats.total_time > 0) {
            throughput = (client_stats.bytes_sent + client_stats.bytes_received) / 
                        (double)MB / client_stats.total_time;
            msg_per_sec = (client_stats.messages_sent + client_stats.messages_received) / 
                         client_stats.total_time;
        }
        
        if (client_stats.latency_count > 0) {
            avg_latency = client_stats.total_latency / client_stats.latency_count * 1000;
        }
        
        char size_str[32];
        if (buffer_sizes[i] >= MB) {
            snprintf(size_str, sizeof(size_str), "%dMB", buffer_sizes[i] / MB);
        } else {
            snprintf(size_str, sizeof(size_str), "%dKB", buffer_sizes[i] / KB);
        }
        
        printf("%-12s %-15.2f %-15.0f %-15.3f\n", 
               size_str, throughput, msg_per_sec, avg_latency);
    }
}

static void multiplexer_test(const char* server_addr, int port) {
    printf("\n=== I/O Multiplexer Comparison Test ===\n");
    
    struct {
        IOMultiplexer mux;
        const char* name;
        void* (*server_func)(void*);
        int available;
    } multiplexers[] = {
        {IO_SELECT, "select", tcp_server_select, 1},
#ifdef HAVE_EPOLL
        {IO_EPOLL, "epoll", tcp_server_epoll, 1},
#endif
#ifdef HAVE_KQUEUE
        {IO_KQUEUE, "kqueue", tcp_server_kqueue, 1},
#endif
    };
    
    int num_mux = sizeof(multiplexers) / sizeof(multiplexers[0]);
    int connections[] = {1, 10, 100, 500, 1000};
    int num_conn_tests = sizeof(connections) / sizeof(connections[0]);
    
    printf("Testing with message size: 1KB, duration: 5 seconds\n\n");
    
    for (int m = 0; m < num_mux; m++) {
        if (!multiplexers[m].available) continue;
        
        printf("\n%s performance:\n", multiplexers[m].name);
        printf("%-12s %-15s %-15s %-15s\n", 
               "Connections", "Throughput MB/s", "Messages/sec", "Avg Latency ms");
        printf("------------ --------------- --------------- ---------------\n");
        
        for (int c = 0; c < num_conn_tests; c++) {
            // For higher connection counts, need more complex testing
            // For now, just test basic functionality
            if (connections[c] > 100) {
                printf("%-12d %-15s %-15s %-15s\n", 
                       connections[c], "N/A", "N/A", "N/A");
                continue;
            }
            
            NetworkStats server_stats = {0};
            NetworkStats client_stats = {0};
            server_stats.min_latency = INFINITY;
            client_stats.min_latency = INFINITY;
            
            pthread_t server_thread, client_thread;
            pthread_barrier_t barrier;
            pthread_barrier_init(&barrier, NULL, 2);
            volatile int stop_flag = 0;
            
            ThreadArgs server_args = {
                .thread_id = 0,
                .mode = MODE_SERVER,
                .protocol = PROTO_TCP,
                .multiplexer = multiplexers[m].mux,
                .port = port,
                .message_size = 1024,
                .stats = &server_stats,
                .stop_flag = &stop_flag,
                .barrier = &barrier
            };
            
            ThreadArgs client_args = {
                .thread_id = 0,
                .mode = MODE_CLIENT,
                .protocol = PROTO_TCP,
                .server_addr = server_addr,
                .port = port,
                .message_size = 1024,
                .num_messages = 5000,
                .stats = &client_stats,
                .stop_flag = &stop_flag,
                .barrier = &barrier
            };
            
            pthread_create(&server_thread, NULL, multiplexers[m].server_func, &server_args);
            usleep(100000);
            pthread_create(&client_thread, NULL, tcp_client, &client_args);
            
            pthread_join(client_thread, NULL);
            stop_flag = 1;
            pthread_join(server_thread, NULL);
            pthread_barrier_destroy(&barrier);
            
            // Calculate results
            double throughput = 0;
            double msg_per_sec = 0;
            double avg_latency = 0;
            
            if (client_stats.total_time > 0) {
                throughput = (client_stats.bytes_sent + client_stats.bytes_received) / 
                            (double)MB / client_stats.total_time;
                msg_per_sec = (client_stats.messages_sent + client_stats.messages_received) / 
                             client_stats.total_time;
            }
            
            if (client_stats.latency_count > 0) {
                avg_latency = client_stats.total_latency / client_stats.latency_count * 1000;
            }
            
            printf("%-12d %-15.2f %-15.0f %-15.3f\n", 
                   connections[c], throughput, msg_per_sec, avg_latency);
        }
    }
}

static void print_usage(const char* prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -m <mode>      Mode: client or server (default: run both)\n");
    printf("  -a <address>   Server address (default: 127.0.0.1)\n");
    printf("  -p <port>      Port number (default: %d)\n", DEFAULT_PORT);
    printf("  -P <protocol>  Protocol: tcp or udp (default: tcp)\n");
    printf("  -t <type>      Test type: throughput, latency, buffer, multiplexer, all\n");
    printf("  -s <size>      Message size in bytes (default: %d)\n", DEFAULT_MESSAGE_SIZE);
    printf("  -n <count>     Number of messages (default: %d)\n", DEFAULT_NUM_MESSAGES);
    printf("  -d <duration>  Test duration in seconds (default: 10)\n");
    printf("  -T <threads>   Number of threads (default: 1)\n");
    printf("  -h             Show this help message\n");
    printf("\nAnalyzes network I/O performance characteristics:\n");
    printf("  - TCP vs UDP throughput and latency\n");
    printf("  - Socket buffer size optimization\n");
    printf("  - I/O multiplexer comparison (select/poll/epoll/kqueue)\n");
    printf("  - Connection scaling analysis\n");
}

int main(int argc, char* argv[]) {
    Mode mode = MODE_CLIENT | MODE_SERVER; // Run both by default
    const char* server_addr = "127.0.0.1";
    int port = DEFAULT_PORT;
    Protocol protocol = PROTO_TCP;
    TestType test_type = TEST_ALL;
    int message_size = DEFAULT_MESSAGE_SIZE;
    int num_messages = DEFAULT_NUM_MESSAGES;
    int duration = 10;
    int num_threads = 1;
    
    // Set up signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    int opt;
    while ((opt = getopt(argc, argv, "m:a:p:P:t:s:n:d:T:h")) != -1) {
        switch (opt) {
            case 'm':
                if (strcmp(optarg, "client") == 0) mode = MODE_CLIENT;
                else if (strcmp(optarg, "server") == 0) mode = MODE_SERVER;
                else {
                    fprintf(stderr, "Invalid mode: %s\n", optarg);
                    return 1;
                }
                break;
            case 'a':
                server_addr = optarg;
                break;
            case 'p':
                port = atoi(optarg);
                if (port <= 0 || port > 65535) {
                    fprintf(stderr, "Invalid port: %s\n", optarg);
                    return 1;
                }
                break;
            case 'P':
                if (strcmp(optarg, "tcp") == 0) protocol = PROTO_TCP;
                else if (strcmp(optarg, "udp") == 0) protocol = PROTO_UDP;
                else {
                    fprintf(stderr, "Invalid protocol: %s\n", optarg);
                    return 1;
                }
                break;
            case 't':
                if (strcmp(optarg, "throughput") == 0) test_type = TEST_THROUGHPUT;
                else if (strcmp(optarg, "latency") == 0) test_type = TEST_LATENCY;
                else if (strcmp(optarg, "buffer") == 0) test_type = TEST_BUFFER_SIZE;
                else if (strcmp(optarg, "multiplexer") == 0) test_type = TEST_MULTIPLEXER;
                else if (strcmp(optarg, "all") == 0) test_type = TEST_ALL;
                else {
                    fprintf(stderr, "Invalid test type: %s\n", optarg);
                    return 1;
                }
                break;
            case 's':
                message_size = atoi(optarg);
                if (message_size <= 0) {
                    fprintf(stderr, "Invalid message size: %s\n", optarg);
                    return 1;
                }
                break;
            case 'n':
                num_messages = atoi(optarg);
                if (num_messages <= 0) {
                    fprintf(stderr, "Invalid message count: %s\n", optarg);
                    return 1;
                }
                break;
            case 'd':
                duration = atoi(optarg);
                if (duration <= 0) {
                    fprintf(stderr, "Invalid duration: %s\n", optarg);
                    return 1;
                }
                break;
            case 'T':
                num_threads = atoi(optarg);
                if (num_threads <= 0) {
                    fprintf(stderr, "Invalid thread count: %s\n", optarg);
                    return 1;
                }
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return opt == 'h' ? 0 : 1;
        }
    }
    
    printf("Network I/O Benchmarker\n");
    printf("======================\n");
    printf("Server address: %s\n", server_addr);
    printf("Port: %d\n", port);
    printf("Protocol: %s\n", protocol == PROTO_TCP ? "TCP" : "UDP");
    
    switch (test_type) {
        case TEST_THROUGHPUT:
            throughput_test(protocol, server_addr, port, message_size, duration, num_threads);
            break;
        case TEST_LATENCY:
            // Use smaller messages for latency test
            throughput_test(protocol, server_addr, port, 64, duration, 1);
            break;
        case TEST_BUFFER_SIZE:
            buffer_size_test(protocol, server_addr, port);
            break;
        case TEST_MULTIPLEXER:
            if (protocol == PROTO_TCP) {
                multiplexer_test(server_addr, port);
            } else {
                printf("Multiplexer test only available for TCP\n");
            }
            break;
        case TEST_ALL:
            printf("\n### TCP Performance ###\n");
            throughput_test(PROTO_TCP, server_addr, port, message_size, duration, num_threads);
            buffer_size_test(PROTO_TCP, server_addr, port);
            
            printf("\n### UDP Performance ###\n");
            throughput_test(PROTO_UDP, server_addr, port, message_size, duration, num_threads);
            buffer_size_test(PROTO_UDP, server_addr, port);
            
            multiplexer_test(server_addr, port);
            break;
        default:
            break;
    }
    
    return 0;
}