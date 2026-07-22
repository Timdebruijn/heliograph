// SPDX-License-Identifier: MIT

#include "state_store.h"

namespace heliograph {

StateStore::StateStore() : current_(std::make_shared<const DeviceState>()) {}

void StateStore::publish(const DeviceState& state) {
    auto copy = std::make_shared<const DeviceState>(state);
    std::lock_guard<std::mutex> lock(m_);
    current_ = std::move(copy);
}

StateHandle StateStore::snapshot() const {
    std::lock_guard<std::mutex> lock(m_);
    return current_;
}

StateStore* DeviceManager::add(const DeviceId& id) {
    std::lock_guard<std::mutex> lock(m_);
    for (auto& e : entries_) {
        if (e.id == id) {
            return e.store.get();
        }
    }
    if (entries_.size() >= kMaxActiveDevices) {
        return nullptr;
    }
    entries_.push_back(Entry{id, std::make_unique<StateStore>()});
    return entries_.back().store.get();
}

std::vector<DeviceId> DeviceManager::devices() const {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<DeviceId> out;
    out.reserve(entries_.size());
    for (const auto& e : entries_) {
        out.push_back(e.id);
    }
    return out;
}

StateHandle DeviceManager::state(const DeviceId& id) const {
    std::lock_guard<std::mutex> lock(m_);
    for (const auto& e : entries_) {
        if (e.id == id) {
            return e.store->snapshot();
        }
    }
    return nullptr;
}

StateStore* DeviceManager::store(const DeviceId& id) {
    std::lock_guard<std::mutex> lock(m_);
    for (auto& e : entries_) {
        if (e.id == id) {
            return e.store.get();
        }
    }
    return nullptr;
}

bool DeviceManager::contains(const DeviceId& id) const {
    std::lock_guard<std::mutex> lock(m_);
    for (const auto& e : entries_) {
        if (e.id == id) {
            return true;
        }
    }
    return false;
}

size_t DeviceManager::size() const {
    std::lock_guard<std::mutex> lock(m_);
    return entries_.size();
}

void DeviceManager::clear() {
    std::lock_guard<std::mutex> lock(m_);
    entries_.clear();
}

}  // namespace heliograph
