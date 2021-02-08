//
//  TPCircularBuffer.h
//  Circular/Ring buffer implementation
//
//  https://github.com/michaeltyson/TPCircularBuffer
//
//  Created by Michael Tyson on 10/12/2011.
//
//  This implementation makes use of a virtual memory mapping technique that inserts a virtual copy
//  of the buffer memory directly after the buffer's end, negating the need for any buffer wrap-around
//  logic. Clients can simply use the returned memory address as if it were contiguous space.
//  
//  The implementation is thread-safe in the case of a single producer and single consumer.
//
//  Virtual memory technique originally proposed by Philip Howard (http://vrb.slashusr.org/), and
//  adapted to Darwin by Kurt Revis (http://www.snoize.com,
//  http://www.snoize.com/Code/PlayBufferedSoundFile.tar.gz).
//
//  Copyright (C) 2012-2013 A Tasty Pixel
//
//  This software is provided 'as-is', without any express or implied
//  warranty.  In no event will the authors be held liable for any damages
//  arising from the use of this software.
//
//  Permission is granted to anyone to use this software for any purpose,
//  including commercial applications, and to alter it and redistribute it
//  freely, subject to the following restrictions:
//
//  1. The origin of this software must not be misrepresented; you must not
//     claim that you wrote the original software. If you use this software
//     in a product, an acknowledgment in the product documentation would be
//     appreciated but is not required.
//
//  2. Altered source versions must be plainly marked as such, and must not be
//     misrepresented as being the original software.
//
//  3. This notice may not be removed or altered from any source distribution.
//

#ifndef TPCircularBuffer_h
#define TPCircularBuffer_h

#include <stdbool.h>
#include <string.h>
#include <assert.h>

#ifdef __cplusplus
    extern "C++" {
        #include <atomic>
        using namespace std;
    }
#else
    #include <stdatomic.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
    
typedef struct {
    void              *buffer;
    int32_t           length;
    int32_t           tail;
    int32_t           head;
    atomic_int        fillCount;
    bool              atomic;
} TPCircularBuffer;

/*!
 * Initialise buffer
 *
 *  Note that the length is advisory only: Because of the way the
 *  memory mirroring technique works, the true buffer length will
 *  be multiples of the device page size (e.g. 4096 bytes).
 *
 *  If you intend to use the AudioBufferList utilities, you should
 *  always allocate a bit more space than you need for pure audio
 *  data, so there's room for the metadata. How much extra is required
 *  depends on how many AudioBufferList structures are used, which is
 *  a function of how many audio frames each buffer holds. A good rule
 *  of thumb is to add 15%, or at least another 2048 bytes or so.
 *
 * @param buffer Circular buffer
 * @param length Length of buffer
 */
#define TPCircularBufferInit(buffer, length) \
    _TPCircularBufferInit(buffer, length, sizeof(*buffer))
bool _TPCircularBufferInit(TPCircularBuffer *buffer, int32_t length, size_t structSize);

/*!
 * Cleanup buffer
 *
 *  Releases buffer resources.
 */
void TPCircularBufferCleanup(TPCircularBuffer *buffer);

/*!
 * Clear buffer
 *
 *  Resets buffer to original, empty state.
 *
 *  This is safe for use by consumer while producer is accessing buffer.
 */
void TPCircularBufferClear(TPCircularBuffer *buffer);
    
/*!
 * Set the atomicity
 *
 *  If you set the atomiticy to false using this method, the buffer will
 *  not use atomic operations. This can be used to give the compiler a little
 *  more optimisation opportunities when the buffer is only used on one thread.
 *
 *  Important note: Only set this to false if you know what you're doing!
 *
 *  The default value is true (the buffer will use atomic operations).
 *
 * @param buffer Circular buffer
 * @param atomic Whether the buffer is atomic (default true)
 */
void TPCircularBufferSetAtomic(TPCircularBuffer *buffer, bool atomic);

#pragma mark - Reading (consuming)

/*!
 * Access end of buffer
 *
 *  This gives you a pointer to the end of the buffer, ready
 *  for reading, and the number of available bytes to read.
 *
 * @param buffer Circular buffer
 * @param availableBytes On output, the number of bytes ready for reading
 * @return Pointer to the first bytes ready for reading, or NULL if buffer is empty
 */
static __inline__ __attribute__((always_inline)) void *TPCircularBufferTail(const TPCircularBuffer *buffer,
                                                                            int32_t *availableBytes) {
    int fillCount = (buffer->atomic ?
                     atomic_load_explicit(&buffer->fillCount, memory_order_acquire) :
                     buffer->fillCount);
    *availableBytes = (fillCount <= 0 ? 0 : fillCount);

    if ( *availableBytes == 0 ) return NULL;
    return (void *)((char *)buffer->buffer + buffer->tail);
}

/*!
 * Consume bytes in buffer
 *
 *  This frees up the just-read bytes, ready for writing again.
 *
 * @param buffer Circular buffer
 * @param amount Number of bytes to consume
 */
static __inline__ __attribute__((always_inline)) void TPCircularBufferConsume(TPCircularBuffer *buffer,
                                                                              int32_t amount) {
    buffer->tail = (buffer->tail + amount) % buffer->length;
    if ( buffer->atomic ) {
        atomic_fetch_sub_explicit(&buffer->fillCount, amount, memory_order_acq_rel);
    } else {
        buffer->fillCount -= amount;
    }
}

#pragma mark - Writing (producing)

/*!
 * Access front of buffer
 *
 *  This gives you a pointer to the front of the buffer, ready
 *  for writing, and the number of available bytes to write.
 *
 * @param buffer Circular buffer
 * @param availableBytes On output, the number of bytes ready for writing
 * @param discardBytes On output, the number of bytes to discard before writing
 * @return Pointer to the first bytes ready for writing, or NULL if buffer is full
 */
static __inline__ __attribute__((always_inline)) void *TPCircularBufferHead(const TPCircularBuffer *buffer,
                                                                            int32_t *availableBytes,
                                                                            int32_t *discardBytes) {
    int fillCount = (buffer->atomic ?
                     atomic_load_explicit(&buffer->fillCount, memory_order_acquire) :
                     buffer->fillCount);
    if (fillCount <= 0) {
        *availableBytes = buffer->length;
        *discardBytes = -fillCount;
    } else {
        *availableBytes = buffer->length - fillCount;
        *discardBytes = 0;
    }

    if ( *availableBytes == 0 ) return NULL;
    return (void *)((char *)buffer->buffer + buffer->head);
}

/*!
 * Produce bytes in buffer
 *
 *  This marks the given section of the buffer ready for reading.
 *
 * @param buffer Circular buffer
 * @param amount Number of bytes to produce
 * @return Number of bytes ready for reading before the operation
 */
static __inline__ __attribute__((always_inline)) int TPCircularBufferProduce(TPCircularBuffer *buffer,
                                                                              int32_t amount) {
    buffer->head = (buffer->head + amount) % buffer->length;
    int previousFillCount;
    if ( buffer->atomic ) {
        previousFillCount = atomic_fetch_add_explicit(&buffer->fillCount, amount, memory_order_acq_rel);
    } else {
        previousFillCount = buffer->fillCount;
        buffer->fillCount += amount;
    }
    assert(previousFillCount + amount <= buffer->length);
    
    return previousFillCount;
}

/*!
 * Helper routine to copy bytes to buffer
 *
 *  This copies the given bytes to the buffer, and marks them ready for reading.
 *
 * @param buffer Circular buffer
 * @param src Source buffer
 * @param len Number of bytes in source buffer
 * @return true if bytes copied, false if there was insufficient space
 */
static __inline__ __attribute__((always_inline)) bool TPCircularBufferProduceBytes(TPCircularBuffer *buffer,
                                                                                   const void *src,
                                                                                   int32_t len) {
    int32_t space, discard;
    void *ptr = TPCircularBufferHead(buffer, &space, &discard);
    if ( space < len - discard ) return false;
    memcpy(ptr + discard, src + discard, len - discard);
    TPCircularBufferProduce(buffer, len);
    return true;
}

#pragma mark - Deprecated

/*!
 * Deprecated method
 */
static __inline__ __attribute__((always_inline))
__deprecated_msg("use TPCircularBufferSetAtomic(false) and TPCircularBufferConsume instead")
void TPCircularBufferConsumeNoBarrier(TPCircularBuffer *buffer, int32_t amount) {
    buffer->tail = (buffer->tail + amount) % buffer->length;
    buffer->fillCount -= amount;
}

/*!
 * Deprecated method
 */
static __inline__ __attribute__((always_inline))
__deprecated_msg("use TPCircularBufferSetAtomic(false) and TPCircularBufferProduce instead")
void TPCircularBufferProduceNoBarrier(TPCircularBuffer *buffer, int32_t amount) {
    buffer->head = (buffer->head + amount) % buffer->length;
    buffer->fillCount += amount;
    assert(buffer->fillCount <= buffer->length);
}

#ifdef __cplusplus
}
#endif

#endif
