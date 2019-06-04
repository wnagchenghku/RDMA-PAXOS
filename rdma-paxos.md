1. `sudo apt-get install libev-dev libconfig-dev libdb-dev`
2. `export PAXOS_ROOT=<absolute path of RDMA-PAXOS>`
3. Compile.
   - `cd target`
   - `make clean; make`

Change `nodes` in /benchmarks/run.sh and run the benchmark:
```
./benchmarks/run.sh app=redis
```

Check the client clt.log on node[-2]

To switch to the original version: e7992b1ab9f74219fac25fc4f35821de0e504c73

To enable debug mode, set `DEBUGOPT := 1` in makefile.init

When DARE calls `rc_write_remote_logs()` (to commit new entries), it will loop until it commits (Note `wait_for_commit == 1`).

> Or it has looped for `threshold` times.

In `rc_write_remote_logs()`, if it is not commited (i.e., committed != 1), it will loop back to loop_for_commit.
And then invoke `update_remote_logs()` again.

The first time DARE invokes `update_remote_logs()`, it will write the entries to remote.

The second time DARE invokes `update_remote_logs()`, it will update the end pointer on the remote.

Finally, to commit, the leader sets the local commit pointer to the minimum tail pointer among at
least a majority of servers and then sets `committed = 1`. Then DARE can break `rc_write_remote_logs()`.

In `update_remote_logs()`, this logic:
```
local_buf[0] = SRV_DATA->log->entries + *remote_end;
local_buf_len[0] = SRV_DATA->log->len - *remote_end;
local_buf[1] = SRV_DATA->log->entries;
local_buf_len[1] = SRV_DATA->log->end;
server->send_count = 2;
```
means the circular buffer is wrapped.

> Every server applies the RSM operations stored in the log entries between its apply and commit pointers; once an operation is applied, the server advances its apply pointer. When an RSM operation is applied by all the non-faulty servers in the group, the entry containing it can be removed from the log. Thus, the leader advances its own head pointer to the smallest apply pointer in the group; then, it appends to the log an HEAD entry that contains the new head pointer. Servers update their head pointer only when they encounter a committed HEAD entry;


For the CSM requests, if there is not enough room for the cmd, DARE just appends the whole entry at the beginning of the log. In applying the committed entries, if there is not enough rooom, DARE just sets apply=0.

When wrap around, we first append the entry head at the end, which has the length of the cmd, then append the whole entry at the beginning. In this case, we can know when there is not enough room for the cmd.

