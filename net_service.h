#pragma once

// The host's network on its own thread (netcode milestone 6): ENet is polled
// there about every millisecond, not once per frame of the frontend's loop,
// so the round trip doesn't carry up to a frame of waiting, high rates get
// drained in time, and (later) frames are encoded and sent there without
// touching the frontend's loop: AUTO frame delay and run-ahead keep their
// time.
//
// What net_access_poll reports is copied into a queue the frontend drains
// once per loop (net_service_next). Everything else in NetAccess (sending,
// pairing, the device list, levels) is used as before, between
// net_service_lock and net_service_unlock: one mutex guards the whole
// NetAccess, the thread takes it only for its short polls.
//
// Threading: std::thread with a plain function, one std::mutex. The thread
// never calls the frontend or the core.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "net_access.h"

constexpr uint32_t NET_SERVICE_QUEUE = 1024;     // events between two drains (input packets included)
constexpr uint32_t NET_SERVICE_POLL_MS = 1;

struct NetServiceSlot
{
    NetAccessEvent       event;
    std::vector<uint8_t> data;   // the event's payload, copied (reused: grows, never shrinks)
};

struct NetService
{
    NetAccess      access;               // guarded by `lock` once started
    std::mutex     lock;
    std::thread    thread;
    bool           running = false;      // read by the thread under `lock`
    NetServiceSlot queue[NET_SERVICE_QUEUE];
    uint32_t       head = 0;             // the oldest queued event
    uint32_t       count = 0;
    uint32_t       dropped = 0;          // events lost to a full queue (logged)
    std::vector<uint8_t> current;        // the payload of the event net_service_next last gave
};

// Creates the NetAccess (as net_access_create) and starts the thread.
bool net_service_start(NetService* service, uint16_t port, const char* name, const NetKeypair* identity,
                       const char* trust_path);
// Stops the thread and destroys the NetAccess. Safe on a service that
// didn't start.
void net_service_stop(NetService* service);

void net_service_lock(NetService* service);
void net_service_unlock(NetService* service);

// Locked: the next queued event, oldest first; its `data` stays valid until
// the next call. False when the queue is empty.
bool net_service_next(NetService* service, NetAccessEvent* event);
