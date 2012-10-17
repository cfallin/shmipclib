/*
 * shmipclib: shared-memory IPC without going through the kernel.
 *
 * Copyright (C) 2012 Chris Fallin <cfallin@c1f.net>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef _SHMIPCLIB_SHM_H_
#define _SHMIPCLIB_SHM_H_

#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>

/**
 * A shared memory segment. Represents a memory blob that can be mapped
 * into multiple process' address spaces.
 */
class SHM {
    private:
        int _fd;
        void *_p;
        uint64_t _size;
        char _name[256]; // file name in shm namespac3e

    public:
        /**
         * Open or create a shared memory segment with the given name. Segment
         * size is obtained from the existing SHM segment, if any, or set to zero
         * if the segment is created by this call.
         */
        SHM(const char *name) {
            _p = NULL;
            _size = 0;
            strncpy(_name, name, sizeof(_name));
            _fd = shm_open(name, O_RDWR | O_CREAT, 0644);
            if (_fd == -1) return;
            
            struct stat st;
            if (fstat(_fd, &st)) return;

            _size = st.st_size;

            if (_size) {
                _p = mmap(0, _size, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, 0);
            }
        }

        /**
         * Unmap and close the segment. The segment is not deleted in the
         * underlying SHM namespace.
         */
        virtual ~SHM() {
            if (_p) {
                munmap(_p, _size);
                _p = NULL;
            }
            if (_fd != -1) {
                close(_fd);
                _fd = -1;
            }
        }

        /**
         * Unlink the segment from the underlying SHM namespace (virtual
         * filesystem).
         */
        void unlink() {
            if (_p) {
                munmap(_p, _size);
                _p = NULL;
            }
            if (_fd != -1) {
                close(_fd);
                shm_unlink(_name);
                _fd = -1;
            }
        }

        /**
         * Resize the underlying segment. Returns true for success or false
         * for failure.
         *
         * Note that when one process resizes the segment, other processes' mappings'
         * sizes do not change. The processes should have a means to communicate
         * resizes at a higher level. If one process sets a new size, then
         * other processes can later call resize() on their SHM objects with
         * the same size as an argument, and (i) the segment size will be
         * set again (without effect) and (ii) the mapping size will be
         * changed.
         *
         * N.B. that the mapping address will *likely* change when the size is
         * changed. Clients should re-obtain pointers to objects in the segment
         * (via ptr()) after resizing.
         */
        bool resize(uint64_t size) {
            uint64_t oldsize = _size;
            void *endp = (void *) ( ((uint8_t*)_p) + oldsize );
            if (size == oldsize) return true;
            if (_fd == -1) return false;
            size = (size + 0xfff) & ~0xfffULL;
            if (ftruncate(_fd, size)) return false;
            munmap(_p, _size);
            _p = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, 0);
            _size = size;
            return (_p != NULL);
        }

        /**
         * accessor: pointer to mapping of segment in this process' address space.
         *
         * this pointer will likely change after resizes.
         */
        uint8_t *ptr() const { return (uint8_t *)_p; }

        /**
         * accessor: pointer to an offset in this segment's mapping.
         */
        uint8_t *ptr(uint64_t off) const { return (uint8_t *)_p + off; }

        /**
         * accessor: current mapping size. (N.B. notes on resize() -- this size
         * does *not* automatically adjust if another process resizes the segment.)
         */
        uint64_t size() const { return _size; }
};

/**
 * A spinlock in a shared memory segment. Never yields to the OS, but simply
 * busy-waits while the lock is held.
 *
 * *NOT* a recursive spinlock: will deadlock if a thread calls lock() on a spinlock
 * while it already holds that lock (unless the thread knows what it's doing and
 * releases the lock e.g. from a signal handler or another thread/process).
 *
 * It is recommended that spinlocks be created in their own independent cache blocks,
 * to avoid false sharing effects. It is up to the user to work out the necessary
 * alignment for this.
 *
 * Locking hierarchy is the user's responsibility.
 */
class Spinlock {
    private:
        volatile uint64_t *p;

    public:
        Spinlock() {
            p = 0;
        }

        /**
         * create the spinlock object and attach it to a shared-memory segment at
         * a particular offset.
         */
        Spinlock(SHM *s, uint64_t off) {
            init(s, off);
        }

        /**
         * attach the spinlock object to a shared memory segment at a particular
         * offset. Note that the spinlock is *NOT* zeroed; it is the user's responsibility
         * to zero the memory before using the first spinlock.
         */
        void init(SHM *s, uint64_t off) {
            p = (volatile uint64_t *)( s->ptr() + off );
        }

        /**
         * zero (initialize) the spinlock.
         */
        void zero() {
            *p = 0;
        }

        /**
         * acquire the lock. (Does not check if this thread is recursively
         * holding the lock already; will deadlock in that case.)
         */
        void lock() {
            asm volatile
                (
                 // test-and-test-and-set
                 "1: movq $1, %%rdx\n"
                 "2: movq (%%rax), %%rcx\n"
                 "   testq %%rcx, %%rcx\n"
                 "   jnz 2b\n"
                 "   lock xchg (%%rax), %%rdx\n"
                 "   testq %%rdx, %%rdx\n"
                 "   jnz 1b\n"
                 :: "a" (p) : "%rcx", "%rdx"
                );
        }

        /**
         * release the lock. (Does not check if this thread was actually holding
         * the lock.)
         */
        void unlock() {
            *p = 0;
        }
};

/*
 * SHM segment layout:
 *
 * cache block 0:
 *    (offset 0,   size 8): global spinlock (take to change size)
 *    (offset 8,   size 8): size of segment in elements
 *
 * cache block 1:
 *    (offset 64,  size 8): head pointer spinlock
 * cache block 2:
 *    (offset 128, size 8): tail pointer spinlock
 * cache block 3:
 *    (offset 192, size 8): head pointer (index into array)
 * cache block 4:
 *    (offset 256, size 8): tail pointer (index into array)
 * cache block 5...N:
 *    (offset 320, size N): circular buffer of elements
 *
 * lock hierarchy:
 *   head ptr (lowest), tail ptr, global (size) spinlock.
 *
 *   push takes head ptr first; takes tail ptr lock next if it appears
 *   there's no space; if still no space, takes global spinlock and
 *   resizes segment.
 *
 *   pop takes tail ptr; never needs to resize so never takes global spinlock.
 */

/**
 * A message-passing queue through a shared memory segment. Optionally
 * grows dynamically as messages are pushed onto the queue.
 */
template<typename T>
class SHMQueue {
    private:
        SHM *_s;
        Spinlock _sl_global, _sl_head, _sl_tail;
        uint64_t *_nelem, *_head, *_tail;
        T *_array;
        uint64_t _lastsize;

        uint64_t segsize(int elemcount) {
            return 320 + sizeof(T) * elemcount;
        }

        /*
         * checks whether the last element count we saw corresponds to the 
         * element count currently stated in the SHM segment header. If
         * not, resize our mapping and re-grab all of our internal pointers.
         */
        void _internal_resize() {
            if (_lastsize != *_nelem) {
                _s->resize(segsize(*_nelem));
                _sl_global.init(_s, 0);
                _sl_head.init(_s, 64);
                _sl_tail.init(_s, 128);
                _nelem = (uint64_t *)(_s->ptr() + 8);
                _head = (uint64_t *)(_s->ptr() + 192);
                _tail = (uint64_t *)(_s->ptr() + 256);
                _array = (T *)(_s->ptr() + 320);
                _lastsize = *_nelem;
            }
        }

    public:
        SHMQueue(SHM *s, int initsize = 64) {
            _s = s;

            // initially, the segment is of zero size -- set up
            // the segment size and zero the memory.
            if (s->size() == 0) {
                s->resize(segsize(initsize));
                memset(s->ptr(), 0, segsize(initsize));
                _nelem = (uint64_t *)(s->ptr() + 8);
                *_nelem = initsize;
            }

            // grab all our pointers
            _internal_resize();
        }

        /**
         * Push an item onto the queue. If 'expand' is true, then the queue
         * is allowed to dynamically expand. If 'expand' is false, then
         * this method returns false if the queue is full.
         */
        bool push(T& t, bool expand = true) {
            _sl_head.lock();
            // if the head pointer has wrapped around all the way to the tail,
            // then we are full...
            // (N.B.: actually resize when we have one slot left. Otherwise, we can't
            // distinguish (head==tail) -> full from (head==tail) -> empty, i.e.,
            // 0 and 2^N alias each other.)
            if (((*_head + 1) % *_nelem) == *_tail) {

                if (!expand) {
                    _sl_head.unlock();
                    return false;
                }

                _sl_tail.lock();
                // always check expand condition again (i.e. test-and-test-and-set)
                if (((*_head + 1) % *_nelem) == *_tail) {
                    _sl_global.lock();

                    // always double the size.
                    (*_nelem) <<= 1;

                    _internal_resize();

                    // move the part of the array that wrapped around to the second
                    // half (past the old endpoint) so that head > tail again.
                    memcpy(&_array[(*_nelem) >> 1], &_array[0], sizeof(T) * (*_head));
                    (*_head) += ((*_nelem) >> 1);

                    _sl_global.unlock();
                }
                _sl_tail.unlock();
            }

            // move the part of the item array that wrapped around out to the 
            memcpy(&_array[*_head], &t, sizeof(T));
            (*_head) = (*_head + 1) & (*_nelem - 1);
            _sl_head.unlock();

            return true;
        }

        /**
         * Pop an item off the queue. Returns true if an item was returned, and
         * false if the queue was empty.
         */
        bool pop(T* out) {
            _sl_tail.lock();

            // recognize and perform resizes done by other processes.
            if (_lastsize != *_nelem) {
                _sl_global.lock();
                _internal_resize();
                _sl_global.unlock();
            }

            // N.B. from resize path above -- we resize before tail==head, so we never
            // have that tail==head for a full queue, only when empty.
            if (*_tail == *_head) {
                _sl_tail.unlock();
                return false;
            }

            memcpy(out, &_array[*_tail], sizeof(T));
            (*_tail) = (*_tail + 1) & (*_nelem - 1);
            _sl_tail.unlock();
            return true;
        }

        /**
         * Returns true if the queue is currently empty.
         */
        bool empty() {

            // N.B.: no locking required because any use of this method must be
            // within a loop, i.e., spinning while a queue is empty. We are
            // free to return 'false' even if a push is happening nearly
            // simultaneously because we can just as well say that the
            // empty-check came right before the push as right after. i.e. we
            // are serializable with any head/tail pointer update because those
            // updates are single atomic word updates.

            return (*_head == *_tail);
        }
};

#endif
