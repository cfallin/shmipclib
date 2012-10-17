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

#include "shm.h"
#include <stdio.h>
#include <assert.h>

struct Message {
    uint64_t m[8];
};

void produce(SHM *s) {
    printf("Producer starting up\n");

    uint64_t seq = 0;

    SHMQueue<Message> q(s);

    while (seq < 100*1000*1000) {
        if (seq % 1000000 == 0) printf("seq: %ld\n", seq);

        Message m;
        for (int i = 0; i < 8; i++) m.m[i] = seq;
        seq++;

        q.push(m);
    }
}

void consume(SHM *s) {
    printf("Consumer starting up\n");

    uint64_t seq = 0;

    SHMQueue<Message> q(s);

    while (seq < 100*1000*1000) {
        if (seq % 1000000 == 0) printf("seq: %ld\n", seq);

        Message m;

        while (!q.pop(&m)) /* nothing */ ;

        for (int i = 0; i < 8; i++) {
            assert(m.m[i] == seq);
        }

        seq++;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;

    bool producer = !strcmp(argv[1], "-p");
    bool consumer = !strcmp(argv[1], "-c");

    if (!producer && !consumer) return 1;

    SHM *s = new SHM("producer_consumer");

    if (producer) produce(s);
    if (consumer) consume(s);

    s->unlink();
    delete s;

    return 0;
}
