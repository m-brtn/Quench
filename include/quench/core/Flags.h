#pragma once

// Bit flags on a scoped enum: QUENCH_FLAGS(MyFlags) after the enum (at namespace scope) defines |, &, ^, ~, |=, &=
// and any(). The enum lists single bits and may name combinations.

#include <type_traits>

#define QUENCH_FLAGS(E)                                                                                                \
    constexpr E operator|(E a, E b) { return E(std::underlying_type_t<E>(a) | std::underlying_type_t<E>(b)); }         \
    constexpr E operator&(E a, E b) { return E(std::underlying_type_t<E>(a) & std::underlying_type_t<E>(b)); }         \
    constexpr E operator^(E a, E b) { return E(std::underlying_type_t<E>(a) ^ std::underlying_type_t<E>(b)); }         \
    constexpr E operator~(E a) { return E(~std::underlying_type_t<E>(a)); }                                            \
    constexpr E& operator|=(E& a, E b) { return a = a | b; }                                                           \
    constexpr E& operator&=(E& a, E b) { return a = a & b; }                                                           \
    constexpr bool any(E a) { return std::underlying_type_t<E>(a) != 0; }
