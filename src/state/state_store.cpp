// SPDX-License-Identifier: MIT

#include "state_store.h"

#include <algorithm>
#include <utility>

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

const DeviceManager::Entry* DeviceManager::find(const DeviceId& id) const {
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [&id](const Entry& e) { return e.id == id; });
    return it == entries_.end() ? nullptr : &*it;
}

// The const overload does the work; this one casts its result back. The object is genuinely
// non-const on this path, so removing the const that was added a line ago is defined.
DeviceManager::Entry* DeviceManager::find(const DeviceId& id) {
    return const_cast<Entry*>(std::as_const(*this).find(id));
}

StateStore* DeviceManager::add(const DeviceId& id) {
    std::lock_guard<std::mutex> lock(m_);
    if (Entry* existing = find(id)) {
        return existing->store.get();
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
    const Entry* e = find(id);
    return e == nullptr ? nullptr : e->store->snapshot();
}

StateStore* DeviceManager::store(const DeviceId& id) {
    std::lock_guard<std::mutex> lock(m_);
    Entry* e = find(id);
    return e == nullptr ? nullptr : e->store.get();
}

bool DeviceManager::contains(const DeviceId& id) const {
    std::lock_guard<std::mutex> lock(m_);
    return find(id) != nullptr;
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
