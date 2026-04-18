# PES-VCS Lab Report

**Name:** B V Tejas
**SRN:** PES1UG24CS108
**Repository:** https://github.com/Tejas6499/PES1UG24CS108-pes-vcs

## Screenshots
*(Screenshots added separately)*

## Phase 5 - Branching and Checkout

### Q5.1
A branch is a file in .pes/refs/heads/ containing a commit SHA. To implement pes checkout: update .pes/HEAD to ref: refs/heads/<branch>, then walk the target branch tree and write each blob to disk, deleting files absent in the target. This is complex because dozens of files may change non-atomically and a crash mid-checkout leaves the repo inconsistent.

### Q5.2
Compare three values for each file: the index SHA (last staged), the disk SHA (current content), and the target-branch SHA. A conflict exists when disk SHA differs from index SHA AND index SHA differs from target-branch SHA. Detect this by hashing each working-directory file and comparing with index and target tree entries using memcmp on ObjectIDs. If any conflict exists, abort checkout and report the conflicting files.

### Q5.3
In detached HEAD, commits are written to the object store but HEAD contains a raw SHA instead of a branch ref. When you switch back to a branch those commits become unreferenced and will be deleted by GC. To recover: create a new branch pointing to the tip SHA. If the SHA is unknown, scan all commit objects in .pes/objects/ by reading and parsing each one to find commits made during that session.

## Phase 6 - Garbage Collection

### Q6.1
Use mark-and-sweep. Mark phase: seed a reachable set with all tip SHAs from .pes/refs/ and HEAD, then BFS through each commit following tree and parent pointers, adding every SHA to a hash set. Sweep phase: enumerate every file under .pes/objects/, reconstruct its SHA from the path, and delete it if absent from the reachable set. Use a hash table for O(1) membership tests. For 100000 commits with 50 branches expect roughly 3 million objects to visit across both phases.

### Q6.2
Race condition: GC mark phase runs and finds a new blob unreachable because the commit referencing it has not been written yet. GC sweep deletes the blob. The commit then writes successfully but its blob is gone and the repo is corrupt. Git avoids this with a grace period: any object file newer than 2 weeks is never deleted even if unreachable. This window covers any in-progress commit. PES-VCS can adopt the same fix by calling stat() on each candidate deletion and skipping files with mtime within the last 60 seconds.
