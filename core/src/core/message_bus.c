#include <string.h>

#include "core/message_bus.h"

static MessageBus tx_bus;
static MessageBus rx_bus;

static IPCStatus bus_init_one(MessageBus *bus);
static void bus_shutdown_one(MessageBus *bus);
static IPCStatus bus_publish_one(MessageBus *bus, const IPCPacket *packet);
static IPCStatus bus_pop_one(MessageBus *bus, IPCPacket *packet);

static IPCStatus bus_init_one(MessageBus *bus) {
    memset(bus, 0, sizeof(*bus));

    pthread_mutex_init(&bus->mutex, NULL);
    pthread_cond_init(&bus->cond, NULL);

    bus->running = true;

    return IPC_OK;
}

static void bus_shutdown_one(MessageBus *bus) {
    pthread_mutex_lock(&bus->mutex);

    bus->running = false;

    pthread_cond_broadcast(&bus->cond);

    pthread_mutex_unlock(&bus->mutex);

    pthread_mutex_destroy(&bus->mutex);
    pthread_cond_destroy(&bus->cond);
}

static IPCStatus bus_publish_one(MessageBus *bus, const IPCPacket *packet) {
    if (!packet)
        return IPC_ERROR;

    pthread_mutex_lock(&bus->mutex);

    if (bus->count == BUS_QUEUE_SIZE) {
        pthread_mutex_unlock(&bus->mutex);
        return IPC_ERROR;
    }

    bus->packets[bus->tail] = *packet;

    bus->tail = (bus->tail + 1) % BUS_QUEUE_SIZE;
    bus->count++;

    pthread_cond_signal(&bus->cond);

    pthread_mutex_unlock(&bus->mutex);

    return IPC_OK;
}

static IPCStatus bus_pop_one(MessageBus *bus, IPCPacket *packet) {
    if (!packet)
        return IPC_ERROR;

    pthread_mutex_lock(&bus->mutex);

    while (bus->running && bus->count == 0)
        pthread_cond_wait(&bus->cond, &bus->mutex);

    if (!bus->running) {
        pthread_mutex_unlock(&bus->mutex);
        return IPC_DISCONNECTED;
    }

    *packet = bus->packets[bus->head];

    bus->head = (bus->head + 1) % BUS_QUEUE_SIZE;
    bus->count--;

    pthread_mutex_unlock(&bus->mutex);

    return IPC_OK;
}

IPCStatus bus_init(void) {
    bus_init_one(&tx_bus);
    bus_init_one(&rx_bus);

    return IPC_OK;
}

void bus_shutdown(void)
{
    bus_shutdown_one(&tx_bus);
    bus_shutdown_one(&rx_bus);
}

void bus_wakeup_all(void) {
    // Принудительно будим потоки, чтобы они могли проверить флаг завершения
    pthread_mutex_lock(&rx_bus.mutex);
    pthread_cond_broadcast(&rx_bus.cond);
    pthread_mutex_unlock(&rx_bus.mutex);
}

IPCStatus bus_tx_publish(const IPCPacket *packet)
{
    return bus_publish_one(&tx_bus, packet);
}

IPCStatus bus_tx_pop(IPCPacket *packet) {
    return bus_pop_one(&tx_bus, packet);
}

IPCStatus bus_rx_publish(const IPCPacket *packet) {
    return bus_publish_one(&rx_bus, packet);
}

IPCStatus bus_rx_pop(IPCPacket *packet) {
    return bus_pop_one(&rx_bus, packet);
}
