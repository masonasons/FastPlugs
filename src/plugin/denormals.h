#pragma once
// ---------------------------------------------------------------------------
// Denormals flushed to zero for as long as one of these is alive.
//
// A filter ringing down into silence ends up in denormal numbers, which cost
// a hundred times the arithmetic of ordinary ones on most processors. FastRoute
// handles that inside its biquads; this catches everything else, and is what
// every host expects a plugin to do for the length of its process call. The
// previous mode is put back, because the thread belongs to the host.
// ---------------------------------------------------------------------------
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>

class NoDenormals
{
public:
    NoDenormals() : m_old(_mm_getcsr()) { _mm_setcsr(m_old | 0x8040); }   // FTZ | DAZ
    ~NoDenormals() { _mm_setcsr(m_old); }
private:
    unsigned int m_old;
};

#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))

class NoDenormals
{
public:
    NoDenormals()
    {
        asm volatile("mrs %0, fpcr" : "=r"(m_old));
        const uint64_t fz = m_old | (1ull << 24);                         // FZ
        asm volatile("msr fpcr, %0" : : "r"(fz));
    }
    ~NoDenormals() { asm volatile("msr fpcr, %0" : : "r"(m_old)); }
private:
    uint64_t m_old;
};

#else

class NoDenormals {};

#endif
