// core/message_bus.h
#pragma once

#include <stdbool.h>
#include <pthread.h>

#include "ipc/ipc.h"

#define BUS_QUEUE_SIZE 1024

typedef struct {
    IPCPacket packets[BUS_QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool running;
} MessageBus;

IPCStatus bus_init(void);
void bus_shutdown(void);
void bus_wakeup_all(void);
void bus_stop(void); // Функция мягкого перевода шины в состояние выключения

IPCStatus bus_tx_publish(const IPCPacket *packet);
IPCStatus bus_tx_pop(IPCPacket *packet);

IPCStatus bus_rx_publish(const IPCPacket *packet);
IPCStatus bus_rx_pop(IPCPacket *packet);
