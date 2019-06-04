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
