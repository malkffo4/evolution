// ipc/transport.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include "runtime/logging/logging.h"
#include "ipc/ipc.h"
#include "ipc/transport.h"
#include "core/message_bus.h"

#define SOCKET_PATH "/tmp/evolution.sock"
#define MAX_CLIENTS 64

static int listen_fd = -1;
static IPCClient clients[MAX_CLIENTS];
static pthread_t accept_thread;
static volatile int running = 0;

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
        ssize_t rc = send(fd, p, len, 0);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            running = 0;
            LOG_ERROR("send() failed: %s", strerror(errno));
            return IPC_ERROR;
        }
        if (rc == 0) {
            LOG_WARN("IPC connection lost while sending");
            return IPC_DISCONNECTED;
        }
        p += rc;
        len -= (size_t)rc;
    }
    return IPC_OK;
}

static int recv_line(int fd, char *buffer, size_t size) {
    size_t pos = 0;
    while (pos < size - 1) {
        char c;
        ssize_t rc = recv(fd, &c, 1, 0);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            // Не пишем в лог ошибку "Bad file descriptor" при штатном закрытии сокета сервера
            if (errno != EBADF) {
                LOG_ERROR("recv() failed: %s", strerror(errno));
            }
            return IPC_ERROR;
        }
        if (rc == 0) {
            return IPC_DISCONNECTED;
        }
        if (c == '\n')
            break;
        buffer[pos++] = c;
    }
    buffer[pos] = 0;
    return IPC_OK;
}

IPCStatus transport_send_fd(int fd, const IPCPacket *packet) {
    char json[IPC_PAYLOAD_SIZE + 1024];
    if (ipc_packet_to_json(packet, json, sizeof(json)) != IPC_OK)
        return IPC_ERROR;
    if (send_all(fd, json, strlen(json)) != IPC_OK)
        return IPC_DISCONNECTED;
    if (send_all(fd, "\n", 1) != IPC_OK)
        return IPC_DISCONNECTED;
    return IPC_OK;
}

IPCStatus transport_recv_fd(int fd, IPCPacket *packet) {
    char json[IPC_PAYLOAD_SIZE + 1024];
    if (recv_line(fd, json, sizeof(json)) != IPC_OK)
        return IPC_DISCONNECTED;
    return ipc_packet_from_json(json, packet);
}

static IPCClient *alloc_client(void) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (!clients[i].alive) {
            memset(&clients[i], 0, sizeof(clients[i]));
            clients[i].alive = 1;
            return &clients[i];
        }
    }
    return NULL;
}

static void *client_rx_loop(void *arg) {
    // Делаем поток detached, чтобы его ресурсы освобождались автоматически при выходе
    pthread_detach(pthread_self());
    IPCClient *client = arg;
    IPCPacket request;
    IPCPacket response;
    while (client->alive && running) {
        if (transport_recv_fd(client->fd, &request) != IPC_OK)
            break;
        memset(&response, 0, sizeof(response));
        response.id = request.id;
        response.type = IPC_RESPONSE;
        int rc = ipc_dispatch(&request, &response);
        if (rc != IPC_OK) {
            if (response.payload[0] == '\0') {
                response.type = IPC_RESPONSE;
                snprintf(response.name, sizeof(response.name), "error");
                snprintf(response.payload, sizeof(response.payload), "Internal dispatch error");
                response.payload_size = (uint32_t)strlen(response.payload);
            }
        }
        // Отправляем ответ, только если сокет еще живой
        if (client->alive) {
            IPCStatus st = transport_send_fd(client->fd, &response);
            if (st != IPC_OK) {
                break;
            }
        }
    }
    LOG_IPC("Client connection closed fd=%d", client->fd);
    close(client->fd);
    client->fd = -1;
    client->alive = 0;
    return NULL;
}

static void *accept_loop(void *arg) {
    (void)arg;
    while (running) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINVAL || errno == EBADF) {
                break; // Сокет был закрыт главным потоком для остановки сервера
            }
            continue;
        }
        if (!running) {
            close(fd);
            break;
        }
        IPCClient *client = alloc_client();
        if (!client) {
            LOG_WARN("Too many IPC clients connected simultaneously");
            close(fd);
            continue;
        }
        client->fd = fd;
        LOG_IPC("Client connected fd=%d", fd);
        int rc = pthread_create(&client->thread, NULL, client_rx_loop, client);
        if (rc != 0) {
            LOG_ERROR("pthread_create() failed: %s", strerror(errno));
            close(fd);
            client->alive = 0;
        }
    }
    return NULL;
}

IPCStatus transport_server_start(void) {
    struct sockaddr_un addr;
    unlink(SOCKET_PATH);
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        LOG_ERROR("socket() failed: %s", strerror(errno));
        return IPC_ERROR;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("bind() failed: %s", strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return IPC_ERROR;
    }
    if (listen(listen_fd, 64) < 0) {
        LOG_ERROR("listen() failed: %s", strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return IPC_ERROR;
    }
    running = 1;
    int rc = pthread_create(&accept_thread, NULL, accept_loop, NULL);
    if (rc != 0) {
        LOG_ERROR("pthread_create(accept_thread) failed: %s", strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return IPC_ERROR;
    }
    LOG_IPC("IPC server listening on %s", SOCKET_PATH);
    return IPC_OK;
}

void transport_server_stop(void) {
    if (!running) return;
    running = 0;

    // КРИТИЧЕСКОЕ ИСПРАВЛЕНИЕ: Вызываем shutdown() перед close().
    // Это гарантированно заставляет блокирующий вызов accept() в accept_thread проснуться и выйти.
    if (listen_fd >= 0) {
        shutdown(listen_fd, SHUT_RDWR);
        close(listen_fd);
        listen_fd = -1;
    }

    pthread_join(accept_thread, NULL);

    // Мягко закрываем всех клиентов
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i].alive) {
            clients[i].alive = 0;
            if (clients[i].fd >= 0) {
                shutdown(clients[i].fd, SHUT_RDWR);
                close(clients[i].fd);
                clients[i].fd = -1;
            }
        }
    }
    unlink(SOCKET_PATH);
    LOG_IPC("IPC server stopped cleanly");
}
