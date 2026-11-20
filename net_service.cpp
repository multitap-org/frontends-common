#include "net_service.h"

#include <chrono>
#include <cstring>

#include "host_log.h"

// One poll: everything NetAccess has for us, copied into the queue.
static void poll_once(NetService* service)
{
    NetAccessEvent event;
    while (net_access_poll(&service->access, &event))
    {
        if (service->count == NET_SERVICE_QUEUE)
        {
            // The frontend hasn't drained for a long while (a stall): the
            // newest is dropped; pads repeat their last states anyway.
            if (service->dropped++ == 0)
            {
                host_log(LogLevel::warning, "Network: event queue full, events dropped");
            }
            continue;
        }
        NetServiceSlot* slot = &service->queue[(service->head + service->count) % NET_SERVICE_QUEUE];
        slot->event = event;
        slot->data.resize(event.size);
        if (event.size > 0)
        {
            std::memcpy(slot->data.data(), event.data, event.size);
        }
        slot->event.data = nullptr;   // set again when it's handed out
        service->count++;
    }
}

static void run(NetService* service)
{
    for (;;)
    {
        service->lock.lock();
        bool running = service->running;
        if (running)
        {
            poll_once(service);
        }
        service->lock.unlock();
        if (!running)
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(NET_SERVICE_POLL_MS));
    }
}

bool net_service_start(NetService* service, uint16_t port, const char* name, const NetKeypair* identity,
                       const char* trust_path)
{
    service->head = 0;
    service->count = 0;
    service->dropped = 0;
    if (!net_access_create(&service->access, port, name, identity, trust_path))
    {
        net_access_destroy(&service->access);
        return false;
    }
    service->running = true;
    service->thread = std::thread(run, service);
    return true;
}

void net_service_stop(NetService* service)
{
    if (service->thread.joinable())
    {
        service->lock.lock();
        service->running = false;
        service->lock.unlock();
        service->thread.join();
    }
    net_access_destroy(&service->access);
    service->count = 0;
}

void net_service_lock(NetService* service)
{
    service->lock.lock();
}

void net_service_unlock(NetService* service)
{
    service->lock.unlock();
}

bool net_service_next(NetService* service, NetAccessEvent* event)
{
    if (service->count == 0)
    {
        return false;
    }
    NetServiceSlot* slot = &service->queue[service->head];
    service->head = (service->head + 1) % NET_SERVICE_QUEUE;
    service->count--;
    // The payload moves to `current` (no copy): the slot gets `current`'s
    // old buffer to reuse.
    service->current.swap(slot->data);
    *event = slot->event;
    event->data = service->current.empty() ? nullptr : service->current.data();
    return true;
}
