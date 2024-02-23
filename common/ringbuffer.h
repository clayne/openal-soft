#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>

#include "almalloc.h"
#include "flexarray.h"


/* NOTE: This lockless ringbuffer implementation is copied from JACK, extended
 * to include an element size. Consequently, parameters and return values for a
 * size or count are in 'elements', not bytes. Additionally, it only supports
 * single-consumer/single-provider operation.
 */

struct RingBuffer {
private:
#ifdef __cpp_lib_hardware_interference_size
    using std::hardware_destructive_interference_size;
#else
    /* Assume a 64-byte cache line, the most common/likely value. */
    static constexpr std::size_t hardware_destructive_interference_size{64};
#endif
    alignas(hardware_destructive_interference_size) std::atomic<std::size_t> mWritePtr{0u};
    alignas(hardware_destructive_interference_size) std::atomic<std::size_t> mReadPtr{0u};

    alignas(hardware_destructive_interference_size) const std::size_t mWriteSize;
    const std::size_t mSizeMask;
    const std::size_t mElemSize;

    al::FlexArray<std::byte, 16> mBuffer;

public:
    struct Data {
        std::byte *buf;
        std::size_t len;
    };
    using DataPair = std::pair<Data,Data>;

    RingBuffer(const std::size_t writesize, const std::size_t mask, const std::size_t elemsize,
        const std::size_t numbytes)
        : mWriteSize{writesize}, mSizeMask{mask}, mElemSize{elemsize}, mBuffer{numbytes}
    { }

    /** Reset the read and write pointers to zero. This is not thread safe. */
    auto reset() noexcept -> void;

    /**
     * The non-copying data reader. Returns two ringbuffer data pointers that
     * hold the current readable data. If the readable data is in one segment
     * the second segment has zero length.
     */
    [[nodiscard]] auto getReadVector() noexcept -> DataPair;
    /**
     * The non-copying data writer. Returns two ringbuffer data pointers that
     * hold the current writeable data. If the writeable data is in one segment
     * the second segment has zero length.
     */
    [[nodiscard]] auto getWriteVector() noexcept -> DataPair;

    /**
     * Return the number of elements available for reading. This is the number
     * of elements in front of the read pointer and behind the write pointer.
     */
    [[nodiscard]] auto readSpace() const noexcept -> std::size_t
    {
        const std::size_t w{mWritePtr.load(std::memory_order_acquire)};
        const std::size_t r{mReadPtr.load(std::memory_order_acquire)};
        return (w-r) & mSizeMask;
    }

    /**
     * The copying data reader. Copy at most `cnt' elements into `dest'.
     * Returns the actual number of elements copied.
     */
    [[nodiscard]] auto read(void *dest, std::size_t cnt) noexcept -> std::size_t;
    /**
     * The copying data reader w/o read pointer advance. Copy at most `cnt'
     * elements into `dest'. Returns the actual number of elements copied.
     */
    [[nodiscard]] auto peek(void *dest, std::size_t cnt) const noexcept -> std::size_t;
    /** Advance the read pointer `cnt' places. */
    auto readAdvance(std::size_t cnt) noexcept -> void
    { mReadPtr.store(mReadPtr.load(std::memory_order_relaxed)+cnt, std::memory_order_release); }


    /**
     * Return the number of elements available for writing. This is the number
     * of elements in front of the write pointer and behind the read pointer.
     */
    [[nodiscard]] auto writeSpace() const noexcept -> std::size_t
    { return mWriteSize - readSpace(); }

    /**
     * The copying data writer. Copy at most `cnt' elements from `src'. Returns
     * the actual number of elements copied.
     */
    [[nodiscard]] auto write(const void *src, std::size_t cnt) noexcept -> std::size_t;
    /** Advance the write pointer `cnt' places. */
    auto writeAdvance(std::size_t cnt) noexcept -> void
    { mWritePtr.store(mWritePtr.load(std::memory_order_relaxed)+cnt, std::memory_order_release); }

    [[nodiscard]] auto getElemSize() const noexcept -> std::size_t { return mElemSize; }

    /**
     * Create a new ringbuffer to hold at least `sz' elements of `elem_sz'
     * bytes. The number of elements is rounded up to the next power of two
     * (even if it is already a power of two, to ensure the requested amount
     * can be written).
     */
    [[nodiscard]] static
    auto Create(std::size_t sz, std::size_t elem_sz, bool limit_writes) -> std::unique_ptr<RingBuffer>;

    DEF_FAM_NEWDEL(RingBuffer, mBuffer)
};
using RingBufferPtr = std::unique_ptr<RingBuffer>;

#endif /* RINGBUFFER_H */
