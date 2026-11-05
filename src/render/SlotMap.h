#pragma once

// Storage behind the render module's handles: slots reused after removal, a generation per slot so stale handles are
// detected.

#include <quench/render/Handles.h>

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace quench::render::detail {

template <class Id, class T>
class SlotMap {
public:
    Id add(T value) {
        std::uint32_t index;
        if (!m_free.empty()) {
            index = m_free.back();
            m_free.pop_back();
        } else {
            index = std::uint32_t(m_slots.size());
            m_slots.emplace_back();
        }
        Slot& slot = m_slots[index];
        slot.value.emplace(std::move(value));
        ++m_count;
        return {index, slot.generation};
    }

    T* get(Id id) {
        if (id.index >= m_slots.size()) return nullptr;
        Slot& slot = m_slots[id.index];
        return slot.generation == id.generation && slot.value ? &*slot.value : nullptr;
    }
    const T* get(Id id) const { return const_cast<SlotMap*>(this)->get(id); }

    bool remove(Id id) {
        if (!get(id)) return false;
        Slot& slot = m_slots[id.index];
        slot.value.reset();
        slot.generation = slot.generation == 0xffffffffu ? 1u : slot.generation + 1u;
        m_free.push_back(id.index);
        --m_count;
        return true;
    }

    std::uint32_t size() const { return m_count; }
    std::uint32_t capacity() const { return std::uint32_t(m_slots.size()); }
    // Slot access by index for iteration: null for empty slots.
    T* slot(std::uint32_t index) { return m_slots[index].value ? &*m_slots[index].value : nullptr; }
    const T* slot(std::uint32_t index) const { return m_slots[index].value ? &*m_slots[index].value : nullptr; }
    Id idOf(std::uint32_t index) const { return {index, m_slots[index].generation}; }

private:
    struct Slot {
        std::optional<T> value;
        std::uint32_t generation = 1;
    };
    std::vector<Slot> m_slots;
    std::vector<std::uint32_t> m_free;
    std::uint32_t m_count = 0;
};

} // namespace quench::render::detail
