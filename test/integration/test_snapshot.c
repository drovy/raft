#include "../lib/cluster.h"
#include "../lib/runner.h"

/******************************************************************************
 *
 * Fixture
 *
 *****************************************************************************/

struct fixture
{
    FIXTURE_CLUSTER;
};

static void *setUp(const MunitParameter params[], MUNIT_UNUSED void *user_data)
{
    struct fixture *f = munit_malloc(sizeof *f);
    SETUP_CLUSTER(3);
    CLUSTER_BOOTSTRAP;
    CLUSTER_START;
    CLUSTER_ELECT(0);
    return f;
}

static void tearDown(void *data)
{
    struct fixture *f = data;
    TEAR_DOWN_CLUSTER;
    free(f);
}

/******************************************************************************
 *
 * Helper macros
 *
 *****************************************************************************/

/* Set the snapshot threshold on all servers of the cluster */
#define SET_SNAPSHOT_THRESHOLD(VALUE)                            \
    {                                                            \
        unsigned i;                                              \
        for (i = 0; i < CLUSTER_N; i++) {                        \
            raft_set_snapshot_threshold(CLUSTER_RAFT(i), VALUE); \
        }                                                        \
    }

/* Set the snapshot trailing logs number on all servers of the cluster */
#define SET_SNAPSHOT_TRAILING(VALUE)                            \
    {                                                           \
        unsigned i;                                             \
        for (i = 0; i < CLUSTER_N; i++) {                       \
            raft_set_snapshot_trailing(CLUSTER_RAFT(i), VALUE); \
        }                                                       \
    }

/* Set the snapshot timeout on all servers of the cluster */
#define SET_SNAPSHOT_TIMEOUT(VALUE)                                    \
    {                                                                  \
        unsigned i;                                                    \
        for (i = 0; i < CLUSTER_N; i++) {                              \
            raft_set_install_snapshot_timeout(CLUSTER_RAFT(i), VALUE); \
        }                                                              \
    }

static int ioMethodSnapshotPutFail(struct raft_io *raft_io,
                                   unsigned trailing,
                                   struct raft_io_snapshot_put *req,
                                   const struct raft_snapshot *snapshot,
                                   raft_io_snapshot_put_cb cb)
{
    (void)raft_io;
    (void)trailing;
    (void)req;
    (void)snapshot;
    (void)cb;
    return -1;
}

#define SET_FAULTY_SNAPSHOT_PUT()                                        \
    {                                                                    \
        unsigned i;                                                      \
        for (i = 0; i < CLUSTER_N; i++) {                                \
            CLUSTER_RAFT(i)->io->snapshot_put = ioMethodSnapshotPutFail; \
        }                                                                \
    }

static int fsmSnapshotFail(struct raft_fsm *fsm,
                           struct raft_buffer *bufs[],
                           unsigned *n_bufs)
{
    (void)fsm;
    (void)bufs;
    (void)n_bufs;
    return -1;
}

#define SET_FAULTY_SNAPSHOT()                                 \
    {                                                         \
        unsigned i;                                           \
        for (i = 0; i < CLUSTER_N; i++) {                     \
            CLUSTER_RAFT(i)->fsm->snapshot = fsmSnapshotFail; \
        }                                                     \
    }

/******************************************************************************
 *
 * Successfully install a snapshot
 *
 *****************************************************************************/

SUITE(snapshot)

/* Install a snapshot on a follower that has fallen behind. */
TEST(snapshot, installOne, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    (void)params;

    /* Set very low threshold and trailing entries number */
    SET_SNAPSHOT_THRESHOLD(3);
    SET_SNAPSHOT_TRAILING(1);
    CLUSTER_SATURATE_BOTHWAYS(0, 2);

    /* Apply a few of entries, to force a snapshot to be taken. */
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Reconnect the follower and wait for it to catch up */
    CLUSTER_DESATURATE_BOTHWAYS(0, 2);
    CLUSTER_STEP_UNTIL_APPLIED(2, 4, 5000);

    /* Check that the leader has sent a snapshot */
    munit_assert_int(CLUSTER_N_SEND(0, RAFT_IO_INSTALL_SNAPSHOT), ==, 1);
    munit_assert_int(CLUSTER_N_RECV(2, RAFT_IO_INSTALL_SNAPSHOT), ==, 1);
    return MUNIT_OK;
}

/* Install snapshot times out and leader retries */
TEST(snapshot, installOneTimeOut, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    (void)params;

    /* Set very low threshold and trailing entries number */
    SET_SNAPSHOT_THRESHOLD(3);
    SET_SNAPSHOT_TRAILING(1);
    SET_SNAPSHOT_TIMEOUT(200);

    /* Apply a few of entries, to force a snapshot to be taken. Drop all network
     * traffic between servers 0 and 2 in order for AppendEntries RPCs to not be
     * replicated */
    CLUSTER_SATURATE_BOTHWAYS(0, 2);
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Reconnect both servers and set a high disk latency on server 2 so that
     * the InstallSnapshot RPC will time out */
    CLUSTER_SET_DISK_LATENCY(2, 300);
    CLUSTER_DESATURATE_BOTHWAYS(0, 2);

    /* Wait a while and check that the leader has sent a snapshot */
    CLUSTER_STEP_UNTIL_ELAPSED(300);
    munit_assert_int(CLUSTER_N_SEND(0, RAFT_IO_INSTALL_SNAPSHOT), ==, 1);
    munit_assert_int(CLUSTER_N_RECV(2, RAFT_IO_INSTALL_SNAPSHOT), ==, 1);

    /* Wait for the snapshot to be installed */
    CLUSTER_STEP_UNTIL_APPLIED(2, 4, 5000);

    /* Assert that the leader has retried the InstallSnapshot RPC */
    munit_assert_int(CLUSTER_N_SEND(0, RAFT_IO_INSTALL_SNAPSHOT), ==, 2);
    munit_assert_int(CLUSTER_N_RECV(2, RAFT_IO_INSTALL_SNAPSHOT), ==, 2);

    return MUNIT_OK;
}

/* Install snapshot to an offline node */
TEST(snapshot,
     installOneDisconnectedFromBeginningReconnects,
     setUp,
     tearDown,
     0,
     NULL)
{
    struct fixture *f = data;
    (void)params;

    /* Set very low threshold and trailing entries number */
    SET_SNAPSHOT_THRESHOLD(3);
    SET_SNAPSHOT_TRAILING(1);
    SET_SNAPSHOT_TIMEOUT(200);

    /* Apply a few of entries, to force a snapshot to be taken. Disconnect
     * servers 0 and 2 so that the network calls return failure status */
    CLUSTER_DISCONNECT(0, 2);
    CLUSTER_DISCONNECT(2, 0);
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Wait a while so leader detects offline node */
    CLUSTER_STEP_UNTIL_ELAPSED(2000);

    /* Assert that the leader doesn't try sending a snapshot to an offline node
     */
    munit_assert_int(CLUSTER_N_SEND(0, RAFT_IO_INSTALL_SNAPSHOT), ==, 0);
    munit_assert_int(CLUSTER_N_RECV(2, RAFT_IO_INSTALL_SNAPSHOT), ==, 0);

    CLUSTER_RECONNECT(0, 2);
    CLUSTER_RECONNECT(2, 0);
    /* Wait for the snapshot to be installed */
    CLUSTER_STEP_UNTIL_APPLIED(2, 4, 5000);

    /* Assert that the leader has sent an InstallSnapshot RPC */
    munit_assert_int(CLUSTER_N_SEND(0, RAFT_IO_INSTALL_SNAPSHOT), ==, 1);
    munit_assert_int(CLUSTER_N_RECV(2, RAFT_IO_INSTALL_SNAPSHOT), ==, 1);

    return MUNIT_OK;
}

/* Install snapshot to an offline node that went down during operation */
TEST(snapshot,
     installOneDisconnectedDuringOperationReconnects,
     setUp,
     tearDown,
     0,
     NULL)
{
    struct fixture *f = data;
    (void)params;

    /* Set very low threshold and trailing entries number */
    SET_SNAPSHOT_THRESHOLD(3);
    SET_SNAPSHOT_TRAILING(1);
    SET_SNAPSHOT_TIMEOUT(200);

    /* Apply a few of entries */
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Wait for follower to catch up*/
    CLUSTER_STEP_UNTIL_APPLIED(2, 5, 5000);
    /* Assert that the leader hasn't sent an InstallSnapshot RPC  */
    munit_assert_int(CLUSTER_N_SEND(0, RAFT_IO_INSTALL_SNAPSHOT), ==, 0);

    CLUSTER_DISCONNECT(0, 2);
    CLUSTER_DISCONNECT(2, 0);

    /* Wait a while so leader detects offline node */
    CLUSTER_STEP_UNTIL_ELAPSED(2000);

    /* Apply a few more entries */
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Assert that the leader doesn't try sending snapshot to an offline node */
    munit_assert_int(CLUSTER_N_SEND(0, RAFT_IO_INSTALL_SNAPSHOT), ==, 0);
    munit_assert_int(CLUSTER_N_RECV(2, RAFT_IO_INSTALL_SNAPSHOT), ==, 0);

    CLUSTER_RECONNECT(0, 2);
    CLUSTER_RECONNECT(2, 0);
    CLUSTER_STEP_UNTIL_APPLIED(2, 8, 5000);

    /* Assert that the leader has tried sending an InstallSnapshot RPC */
    munit_assert_int(CLUSTER_N_SEND(0, RAFT_IO_INSTALL_SNAPSHOT), ==, 1);
    munit_assert_int(CLUSTER_N_RECV(2, RAFT_IO_INSTALL_SNAPSHOT), ==, 1);

    return MUNIT_OK;
}

/* No snapshots sent to killed nodes */
TEST(snapshot, noSnapshotInstallToKilled, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    (void)params;

    /* Set very low threshold and trailing entries number */
    SET_SNAPSHOT_THRESHOLD(3);
    SET_SNAPSHOT_TRAILING(1);
    SET_SNAPSHOT_TIMEOUT(200);

    /* Kill a server */
    CLUSTER_KILL(2);

    /* Apply a few of entries */
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Wait a while */
    CLUSTER_STEP_UNTIL_ELAPSED(4000);

    /* Assert that the leader hasn't sent an InstallSnapshot RPC  */
    munit_assert_int(CLUSTER_N_SEND(0, RAFT_IO_INSTALL_SNAPSHOT), ==, 0);
    return MUNIT_OK;
}

/* Install snapshot times out and leader retries, afterwards AppendEntries
 * resume */
TEST(snapshot, installOneTimeOutAppendAfter, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    (void)params;

    /* Set very low threshold and trailing entries number */
    SET_SNAPSHOT_THRESHOLD(3);
    SET_SNAPSHOT_TRAILING(1);
    SET_SNAPSHOT_TIMEOUT(200);

    /* Apply a few of entries, to force a snapshot to be taken. Drop all network
     * traffic between servers 0 and 2 in order for AppendEntries RPCs to not be
     * replicated */
    CLUSTER_SATURATE_BOTHWAYS(0, 2);
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Reconnect both servers and set a high disk latency on server 2 so that
     * the InstallSnapshot RPC will time out */
    CLUSTER_SET_DISK_LATENCY(2, 300);
    CLUSTER_DESATURATE_BOTHWAYS(0, 2);

    /* Wait for the snapshot to be installed */
    CLUSTER_STEP_UNTIL_APPLIED(2, 4, 5000);

    /* Append a few entries and check if they are replicated */
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_STEP_UNTIL_APPLIED(2, 5, 5000);

    /* Assert that the leader has retried the InstallSnapshot RPC */
    munit_assert_int(CLUSTER_N_SEND(0, RAFT_IO_INSTALL_SNAPSHOT), ==, 2);

    return MUNIT_OK;
}

/* Install 2 snapshots that both time out and assure the follower catches up */
TEST(snapshot, installMultipleTimeOut, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    (void)params;

    /* Set very low threshold and trailing entries number */
    SET_SNAPSHOT_THRESHOLD(3);
    SET_SNAPSHOT_TRAILING(1);
    SET_SNAPSHOT_TIMEOUT(200);

    /* Apply a few of entries, to force a snapshot to be taken. Drop all network
     * traffic between servers 0 and 2 in order for AppendEntries RPCs to not be
     * replicated */
    CLUSTER_SATURATE_BOTHWAYS(0, 2);
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Reconnect both servers and set a high disk latency on server 2 so that
     * the InstallSnapshot RPC will time out */
    CLUSTER_SET_DISK_LATENCY(2, 300);
    CLUSTER_DESATURATE_BOTHWAYS(0, 2);

    /* Step until the snapshot times out */
    CLUSTER_STEP_UNTIL_ELAPSED(400);

    /* Apply another few of entries, to force a new snapshot to be taken. Drop
     * all traffic between servers 0 and 2 in order for AppendEntries RPCs to
     * not be replicated */
    CLUSTER_SATURATE_BOTHWAYS(0, 2);
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Reconnect the follower */
    CLUSTER_DESATURATE_BOTHWAYS(0, 2);
    CLUSTER_STEP_UNTIL_APPLIED(2, 7, 5000);

    /* Assert that the leader has sent multiple InstallSnapshot RPCs */
    munit_assert_int(CLUSTER_N_SEND(0, RAFT_IO_INSTALL_SNAPSHOT), >=, 2);

    return MUNIT_OK;
}

/* Install 2 snapshots that both time out, launch a few regular AppendEntries
 * and assure the follower catches up */
TEST(snapshot, installMultipleTimeOutAppendAfter, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    (void)params;

    /* Set very low threshold and trailing entries number */
    SET_SNAPSHOT_THRESHOLD(3);
    SET_SNAPSHOT_TRAILING(1);
    SET_SNAPSHOT_TIMEOUT(200);

    /* Apply a few of entries, to force a snapshot to be taken. Drop all network
     * traffic between servers 0 and 2 in order for AppendEntries RPCs to not be
     * replicated */
    CLUSTER_SATURATE_BOTHWAYS(0, 2);
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Reconnect both servers and set a high disk latency on server 2 so that
     * the InstallSnapshot RPC will time out */
    CLUSTER_SET_DISK_LATENCY(2, 300);
    CLUSTER_DESATURATE_BOTHWAYS(0, 2);

    /* Step until the snapshot times out */
    CLUSTER_STEP_UNTIL_ELAPSED(400);

    /* Apply another few of entries, to force a new snapshot to be taken. Drop
     * all traffic between servers 0 and 2 in order for AppendEntries RPCs to
     * not be replicated */
    CLUSTER_SATURATE_BOTHWAYS(0, 2);
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Reconnect the follower */
    CLUSTER_DESATURATE_BOTHWAYS(0, 2);
    /* Append a few entries and make sure the follower catches up */
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_STEP_UNTIL_APPLIED(2, 9, 5000);

    /* Assert that the leader has sent multiple InstallSnapshot RPCs */
    munit_assert_int(CLUSTER_N_SEND(0, RAFT_IO_INSTALL_SNAPSHOT), >=, 2);

    return MUNIT_OK;
}

static bool server_installing_snapshot(struct raft_fixture *f, void *data)
{
    (void)f;
    const struct raft *r = data;
    return r->snapshot.persisting && r->last_stored == 0;
}

static bool server_taking_snapshot(struct raft_fixture *f, void *data)
{
    (void)f;
    const struct raft *r = data;
    return r->snapshot.persisting && r->last_stored != 0;
}

static bool server_snapshot_done(struct raft_fixture *f, void *data)
{
    (void)f;
    const struct raft *r = data;
    return !r->snapshot.persisting;
}

/* Follower receives HeartBeats during the installation of a snapshot */
TEST(snapshot, installSnapshotHeartBeats, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    (void)params;

    /* Set very low threshold and trailing entries number */
    SET_SNAPSHOT_THRESHOLD(3);
    SET_SNAPSHOT_TRAILING(1);
    CLUSTER_SATURATE_BOTHWAYS(0, 1);

    /* Apply a few of entries, to force a snapshot to be taken. */
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Set a large disk latency on the follower, this will allow some
     * heartbeats to be sent during the snapshot installation */
    CLUSTER_SET_DISK_LATENCY(1, 2000);

    munit_assert_uint(CLUSTER_N_RECV(1, RAFT_IO_INSTALL_SNAPSHOT), ==, 0);

    /* Step the cluster until server 1 installs a snapshot */
    const struct raft *r = CLUSTER_RAFT(1);
    CLUSTER_DESATURATE_BOTHWAYS(0, 1);
    CLUSTER_STEP_UNTIL(server_installing_snapshot, (void *)r, 2000);
    munit_assert_uint(CLUSTER_N_RECV(1, RAFT_IO_INSTALL_SNAPSHOT), ==, 1);

    /* Count the number of AppendEntries RPCs received during the snapshot
     * install*/
    unsigned before = CLUSTER_N_RECV(1, RAFT_IO_APPEND_ENTRIES);
    CLUSTER_STEP_UNTIL(server_snapshot_done, (void *)r, 5000);
    unsigned after = CLUSTER_N_RECV(1, RAFT_IO_APPEND_ENTRIES);
    munit_assert_uint(before, <, after);

    /* Check that the InstallSnapshot RPC was not resent */
    munit_assert_uint(CLUSTER_N_RECV(1, RAFT_IO_INSTALL_SNAPSHOT), ==, 1);

    /* Check that the snapshot was applied and we can still make progress */
    CLUSTER_STEP_UNTIL_APPLIED(1, 4, 5000);
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_STEP_UNTIL_APPLIED(1, 6, 5000);

    return MUNIT_OK;
}

/* A follower receives an AppendEntries message while installing a snapshot . */
TEST(snapshot, receiveAppendEntriesWhileInstalling, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    struct raft_transfer transfer;
    int rv;

    /* Set a very low threshold and trailing entries number on server 0. */
    raft_set_snapshot_threshold(CLUSTER_RAFT(0), 2);
    raft_set_snapshot_trailing(CLUSTER_RAFT(0), 1);

    /* Prevent server 2 from receving messages from server 1. */
    CLUSTER_SATURATE_BOTHWAYS(0, 2);

    /* Set a high disk latency on the follower, so it will take a while to
     * complete installing the snapshot. */
    CLUSTER_SET_DISK_LATENCY(2, 1000);

    /* Apply a few of entries, to force a snapshot to be taken. */
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Reconnect the follower and wait for it to receive a snapshot. */
    CLUSTER_DESATURATE_BOTHWAYS(0, 2);
    CLUSTER_STEP_UNTIL(server_installing_snapshot, CLUSTER_RAFT(2), 2000);

    raft_set_snapshot_threshold(CLUSTER_RAFT(0), 64);

    /* Apply a new entry, server 0 won't send it to server 2 since it is waiting
     * for it to complete installing the snapshot. */
    CLUSTER_MAKE_PROGRESS;

    /* Transfer leadership from server 0 to server 1. */
    rv = raft_transfer(CLUSTER_RAFT(0), &transfer, 2, NULL);
    munit_assert_int(rv, ==, 0);
    CLUSTER_STEP_UNTIL_STATE_IS(1, RAFT_LEADER, 1000);

    /* Since server 1 hasn't taken a snapshot, it still has the entries that
     * server 2 is missing, and will try to send them to it. */
    CLUSTER_STEP_UNTIL_APPLIED(2, 4, 2000);

    return MUNIT_OK;
}

/* InstallSnapshot RPC arrives while persisting Entries */
TEST(snapshot, installSnapshotDuringEntriesWrite, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    (void)params;

    /* Set a large disk latency on the follower, this will allow a
     * InstallSnapshot RPC to arrive while the entries are being persisted. */
    CLUSTER_SET_DISK_LATENCY(1, 2000);
    SET_SNAPSHOT_THRESHOLD(3);
    SET_SNAPSHOT_TRAILING(1);

    /* Replicate some entries, these will take a while to persist */
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Make sure leader can't succesfully send any more entries */
    CLUSTER_DISCONNECT(0, 1);
    CLUSTER_MAKE_PROGRESS; /* Snapshot taken here */
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS; /* Snapshot taken here */
    CLUSTER_MAKE_PROGRESS;

    /* Snapshot with index 6 is sent while follower is still writing the entries
     * to disk that arrived before the disconnect. */
    CLUSTER_RECONNECT(0, 1);

    /* Make sure follower is up to date */
    CLUSTER_STEP_UNTIL_APPLIED(1, 7, 5000);
    return MUNIT_OK;
}

static char *fsm_version[] = {"1", "2", NULL};
static MunitParameterEnum fsm_snapshot_async_params[] = {
    {CLUSTER_FSM_VERSION_PARAM, fsm_version},
    {NULL, NULL},
};

/* Follower receives AppendEntries RPCs while taking a snapshot */
TEST(snapshot,
     takeSnapshotAppendEntries,
     setUp,
     tearDown,
     0,
     fsm_snapshot_async_params)
{
    struct fixture *f = data;
    (void)params;

    /* Set very low threshold and trailing entries number */
    SET_SNAPSHOT_THRESHOLD(3);
    SET_SNAPSHOT_TRAILING(1);

    /* Set a large disk latency on the follower, this will allow AppendEntries
     * to be sent while a snapshot is taken */
    CLUSTER_SET_DISK_LATENCY(1, 2000);

    /* Apply a few of entries, to force a snapshot to be taken. */
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Step the cluster until server 1 takes a snapshot */
    const struct raft *r = CLUSTER_RAFT(1);
    CLUSTER_STEP_UNTIL(server_taking_snapshot, (void *)r, 3000);

    /* Send AppendEntries RPCs while server 1 is taking a snapshot */
    static struct raft_apply reqs[5];
    for (int i = 0; i < 5; i++) {
        CLUSTER_APPLY_ADD_X(CLUSTER_LEADER, &reqs[i], 1, NULL);
    }
    CLUSTER_STEP_UNTIL(server_snapshot_done, (void *)r, 5000);

    /* Make sure the AppendEntries are applied and we can make progress */
    CLUSTER_STEP_UNTIL_APPLIED(1, 9, 5000);
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_STEP_UNTIL_APPLIED(1, 11, 5000);
    return MUNIT_OK;
}

TEST(snapshot,
     takeSnapshotSnapshotPutFail,
     setUp,
     tearDown,
     0,
     fsm_snapshot_async_params)
{
    struct fixture *f = data;
    (void)params;

    SET_FAULTY_SNAPSHOT_PUT();

    /* Set very low threshold and trailing entries number */
    SET_SNAPSHOT_THRESHOLD(3);
    SET_SNAPSHOT_TRAILING(1);

    /* Apply a few of entries, to force a snapshot to be taken. */
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* No crash or leaks have occurred */
    return MUNIT_OK;
}

TEST(snapshot, takeSnapshotFail, setUp, tearDown, 0, fsm_snapshot_async_params)
{
    struct fixture *f = data;
    (void)params;

    SET_FAULTY_SNAPSHOT();

    /* Set very low threshold and trailing entries number */
    SET_SNAPSHOT_THRESHOLD(3);
    SET_SNAPSHOT_TRAILING(1);

    /* Apply a few of entries, to force a snapshot to be taken. */
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* No crash or leaks have occurred */
    return MUNIT_OK;
}

/* A follower doesn't convert to candidate state while it's installing a
 * snapshot. */
TEST(snapshot, snapshotBlocksCandidate, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    (void)params;

    /* Set very low threshold and trailing entries number */
    SET_SNAPSHOT_THRESHOLD(3);
    SET_SNAPSHOT_TRAILING(1);

    /* Apply a few of entries, to force a snapshot to be taken. Drop all network
     * traffic between servers 0 and 2 in order for AppendEntries RPCs to not be
     * replicated */
    CLUSTER_SATURATE_BOTHWAYS(0, 2);
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Reconnect both servers and set a high disk latency on server 2 */
    CLUSTER_SET_DISK_LATENCY(2, 5000);
    CLUSTER_DESATURATE_BOTHWAYS(0, 2);

    /* Wait a while and check that the leader has sent a snapshot */
    CLUSTER_STEP_UNTIL_ELAPSED(500);
    munit_assert_int(CLUSTER_N_SEND(0, RAFT_IO_INSTALL_SNAPSHOT), ==, 1);
    munit_assert_int(CLUSTER_N_RECV(2, RAFT_IO_INSTALL_SNAPSHOT), ==, 1);

    /* Disconnect the servers again so that heartbeats, etc. won't arrive */
    CLUSTER_SATURATE_BOTHWAYS(0, 2);
    munit_assert_int(CLUSTER_STATE(2), ==, RAFT_FOLLOWER);
    munit_assert_true(CLUSTER_RAFT(2)->snapshot.persisting);
    CLUSTER_STEP_UNTIL_ELAPSED(4000);
    munit_assert_int(CLUSTER_STATE(2), ==, RAFT_FOLLOWER);
    return MUNIT_OK;
}

/* An UNAVAILABLE node doesn't install snapshots. */
TEST(snapshot, unavailableDiscardsSnapshot, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    (void)params;

    /* FIXME: this test uses raft_fixture_make_unavailable(), which is
     * incompatible with the v1 API, since it calls convertToUnavailable()
     * directly, bypassing raft_step(). */
    return MUNIT_SKIP;

    /* Set very low threshold and trailing entries number */
    SET_SNAPSHOT_THRESHOLD(3);
    SET_SNAPSHOT_TRAILING(1);

    /* Apply a few of entries, to force a snapshot to be taken. Drop all network
     * traffic between servers 0 and 2 in order for AppendEntries RPCs to not be
     * replicated */
    CLUSTER_SATURATE_BOTHWAYS(0, 2);
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Reconnect both servers */
    CLUSTER_SET_DISK_LATENCY(2, 600);
    CLUSTER_DESATURATE_BOTHWAYS(0, 2);

    /* Wait a while and check that the leader has sent a snapshot */
    CLUSTER_STEP_UNTIL_ELAPSED(500);
    munit_assert_int(CLUSTER_N_SEND(0, RAFT_IO_INSTALL_SNAPSHOT), ==, 1);
    munit_assert_int(CLUSTER_N_RECV(2, RAFT_IO_INSTALL_SNAPSHOT), ==, 1);
    raft_fixture_make_unavailable(&f->cluster, 2);
    CLUSTER_STEP_UNTIL_ELAPSED(500);
    munit_assert_uint64(raft_last_applied(CLUSTER_RAFT(2)), ==, 1);
    return MUNIT_OK;
}

/* A new term starts while a node is installing a snapshot. */
TEST(snapshot, newTermWhileInstalling, setUp, tearDown, 0, NULL)
{
    struct fixture *f = data;
    raft_term term;
    (void)params;

    /* Set very low threshold and trailing entries number */
    SET_SNAPSHOT_THRESHOLD(3);
    SET_SNAPSHOT_TRAILING(1);

    /* Apply a few of entries, to force a snapshot to be taken. Drop all network
     * traffic between servers 0 and 2 in order for AppendEntries RPCs to not be
     * replicated */
    CLUSTER_SATURATE_BOTHWAYS(0, 2);
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;
    CLUSTER_MAKE_PROGRESS;

    /* Set a very high disk latency so server 2 will take a lot of time to
     * install the snapshot and the leader will have stepped down in the
     * meantime. */
    CLUSTER_SET_DISK_LATENCY(2, 3000);

    /* Reconnect both servers, so server 2 will receive a snapshot. */
    CLUSTER_DESATURATE_BOTHWAYS(0, 2);

    /* Wait a while and check that the leader has sent a snapshot */
    CLUSTER_STEP_UNTIL_ELAPSED(500);
    munit_assert_int(CLUSTER_N_SEND(0, RAFT_IO_INSTALL_SNAPSHOT), ==, 1);
    munit_assert_int(CLUSTER_N_RECV(2, RAFT_IO_INSTALL_SNAPSHOT), ==, 1);

    /* Save the current term of server 2, we'll check later that it has
     * changed. */
    term = CLUSTER_TERM(2);

    /* Force a new term to start */
    CLUSTER_DEPOSE;
    CLUSTER_ELECT(1);

    /* Server 2's last applied is still behind, meaning it's still persisting
     * the snapshot, however its term has been updated in the meantime. */
    munit_assert_int(CLUSTER_LAST_APPLIED(2), ==, 1);
    munit_assert_int(CLUSTER_TERM(2), ==, term + 1);

    CLUSTER_STEP_UNTIL_ELAPSED(1000);

    /* Server 2 has finished installing the snapshot. */
    munit_assert_int(CLUSTER_LAST_APPLIED(2), ==, 3);

    return MUNIT_OK;
}
