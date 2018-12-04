#include "../../include/raft.h"

#include "../../src/io_queue.h"

#include "../lib/heap.h"
#include "../lib/munit.h"

/**
 * Helpers
 */

struct fixture
{
    struct raft_heap heap;
    struct raft_io_queue queue;
};

/**
 * Push a new request to the queue of the given fixture, and assert that the
 * operation succeeded.
 */
#define __push(F, ID)                            \
    {                                            \
        int rv;                                  \
                                                 \
        rv = raft_io_queue__push(&F->queue, ID); \
        munit_assert_int(rv, ==, 0);             \
    }

/**
 * Assert the size of the queue of the given fixture.
 */
#define __assert_size(F, SIZE)                     \
    {                                              \
        munit_assert_int(F->queue.size, ==, SIZE); \
    }

/**
 * Assert the type of the I'th request in the queue of the given fixture.
 */
#define __assert_type(F, I, TYPE)                                       \
    {                                                                   \
        munit_assert_int(F->queue.requests[I].type, ==, TYPE); \
    }

/**
 * Setup and tear down
 */

static void *setup(const MunitParameter params[], void *user_data)
{
    struct fixture *f = munit_malloc(sizeof *f);

    (void)user_data;

    test_heap_setup(params, &f->heap);

    raft_io_queue__init(&f->queue);

    return f;
}

static void tear_down(void *data)
{
    struct fixture *f = data;

    raft_io_queue__close(&f->queue);

    test_heap_tear_down(&f->heap);

    free(f);
}

/**
 * raft_io_queue__push
 */

/* When the first request is enqueued, the underlying array gets created. */
static MunitResult test_push_first(const MunitParameter params[], void *data)
{
    struct fixture *f = data;
    unsigned id;

    (void)params;

    __push(f, &id);

    munit_assert_int(id, ==, 1);

    __assert_size(f, 2);
    __assert_type(f, 1, RAFT_IO_NULL);

    return MUNIT_OK;
}

/* When a second request is enqueued, there's no need to grow the array. */
static MunitResult test_push_second(const MunitParameter params[], void *data)
{
    struct fixture *f = data;
    struct raft_io_request *request;
    unsigned id1;
    unsigned id2;

    (void)params;

    __push(f, &id1);

    /* Mark the request as in use. */
    request = raft_io_queue_get(&f->queue, id1);
    request->type = RAFT_IO_WRITE_LOG;

    /* Now the queue has 2 slots, one is in use an the other not. */
    __assert_type(f, 0, RAFT_IO_WRITE_LOG);
    __assert_type(f, 1, RAFT_IO_NULL);

    /* Configure the allocator to fail at the next malloc attempt (which won't
     * be made). */
    test_heap_fault_config(&f->heap, 0, 1);
    test_heap_fault_enable(&f->heap);

    /* A second push is successful */
    __push(f, &id2);

    munit_assert_int(id2, ==, 2);

    __assert_size(f, 2);

    raft_io_queue__pop(&f->queue, id1);

    return MUNIT_OK;
}

/* When third request is enqueued, we need to grow the array again. */
static MunitResult test_push_third(const MunitParameter params[], void *data)
{
    struct fixture *f = data;
    struct raft_io_request *request;
    unsigned id1;
    unsigned id2;
    unsigned id3;

    (void)params;

    /* Push two requests and mark them as in use. */
    __push(f, &id1);

    request = raft_io_queue_get(&f->queue, id1);
    request->type = RAFT_IO_WRITE_LOG;

    __push(f, &id2);

    request = raft_io_queue_get(&f->queue, id2);
    request->type = RAFT_IO_WRITE_LOG;

    /* Push a third request. */
    __push(f, &id3);

    munit_assert_int(id3, ==, 3);

    __assert_size(f, 6);

    raft_io_queue__pop(&f->queue, id1);
    raft_io_queue__pop(&f->queue, id2);

    return MUNIT_OK;
}

/* Test out of memory conditions. */
static MunitResult test_push_oom(const MunitParameter params[], void *data)
{
    struct fixture *f = data;
    unsigned id;
    int rv;

    (void)params;

    test_heap_fault_config(&f->heap, 0, 1);
    test_heap_fault_enable(&f->heap);

    rv = raft_io_queue__push(&f->queue, &id);
    munit_assert_int(rv, ==, RAFT_ERR_NOMEM);

    return MUNIT_OK;
}

static MunitTest push_tests[] = {
    {"/first", test_push_first, setup, tear_down, 0, NULL},
    {"/second", test_push_second, setup, tear_down, 0, NULL},
    {"/third", test_push_third, setup, tear_down, 0, NULL},
    {"/oom", test_push_oom, setup, tear_down, 0, NULL},
    {NULL, NULL, NULL, NULL, 0, NULL},
};

/**
 * raft_io_queue__pop
 */

/* When a request is popped, it's slot is marked as free. */
static MunitResult test_pop(const MunitParameter params[], void *data)
{
    struct fixture *f = data;
    struct raft_io_request *request;
    unsigned id;

    (void)params;

    __push(f, &id);

    request = raft_io_queue_get(&f->queue, id);
    request->type = RAFT_IO_WRITE_LOG;

    raft_io_queue__pop(&f->queue, id);

    /* The size of the queue is still 2, but both requests types are null. */
    __assert_size(f, 2);
    __assert_type(f, 0, RAFT_IO_NULL);
    __assert_type(f, 1, RAFT_IO_NULL);

    return MUNIT_OK;
}

static MunitTest pop_tests[] = {
    {"/", test_pop, setup, tear_down, 0, NULL},
    {NULL, NULL, NULL, NULL, 0, NULL},
};

/**
 * Test suite
 */

MunitSuite raft_io_queue_suites[] = {
    {"/push", push_tests, NULL, 1, 0},
    {"/pop", pop_tests, NULL, 1, 0},
    {NULL, NULL, NULL, 0, 0},
};
