// =============================================================================
// AI Packs System - Event Bus
// Publish/Subscribe event system for decoupled communication
// =============================================================================
#pragma once

#include "types.hpp"
#include <unordered_map>
#include <vector>
#include <mutex>
#include <algorithm>

namespace aipack {

class EventBus {
public:
    using SubscriptionId = uint64_t;

    static EventBus& instance() {
        static EventBus bus;
        return bus;
    }

    SubscriptionId subscribe(EventType type, EventHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto id = nextId_++;
        subscribers_[type].push_back({id, std::move(handler)});
        return id;
    }

    SubscriptionId subscribeAll(EventHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto id = nextId_++;
        globalSubscribers_.push_back({id, std::move(handler)});
        return id;
    }

    void unsubscribe(SubscriptionId id) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [type, subs] : subscribers_) {
            subs.erase(
                std::remove_if(subs.begin(), subs.end(),
                    [id](const Subscription& s) { return s.id == id; }),
                subs.end());
        }
        globalSubscribers_.erase(
            std::remove_if(globalSubscribers_.begin(), globalSubscribers_.end(),
                [id](const Subscription& s) { return s.id == id; }),
            globalSubscribers_.end());
    }

    void publish(const Event& event) {
        std::vector<EventHandler> handlers;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = subscribers_.find(event.type);
            if (it != subscribers_.end()) {
                for (auto& sub : it->second)
                    handlers.push_back(sub.handler);
            }
            for (auto& sub : globalSubscribers_)
                handlers.push_back(sub.handler);
        }
        // Call handlers outside lock to prevent deadlocks
        for (auto& handler : handlers) {
            handler(event);
        }
    }

    void publish(EventType type, const std::string& source,
                 const std::string& message, Properties data = {}) {
        Event event{type, source, message, std::move(data)};
        publish(event);
    }

private:
    EventBus() = default;

    struct Subscription {
        SubscriptionId id;
        EventHandler handler;
    };

    std::mutex mutex_;
    SubscriptionId nextId_ = 1;
    std::unordered_map<EventType, std::vector<Subscription>> subscribers_;
    std::vector<Subscription> globalSubscribers_;
};

} // namespace aipack
