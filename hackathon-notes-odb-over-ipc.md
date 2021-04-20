# Hackathon Results

(_I'm going to write up my results here in MD and attach it to my notes, rather than creating a separate slide deck that will just get lost._)

## Title Page

Hackathon Project: Git ODB over IPC
@jeffhostetler, Git-Client Team
April  12-16, 2021

https://github.com/jeffhostetler/git/pull/12

Based upon a suggestion from @derrickstolee

## Questions

1. Can we improve the performance of local Git commands with a local object database (ODB) daemon?
  - All object lookups are modified to ask the daemon rather than directly accessing the disk.
  - The daemon responds to client object requests, accesses the disk, and provides advanced cacheing.
2. Can we build upon the "Simple IPC" layer and thread pool mechanism created for FSMonitor?

## ODB Background

Individual objects (commits, trees, blobs) are stored in a highly-compacted and storage-efficient set of files on disk:

1. Shared packfiles (`*.pack`, `*.idx`, and `*.midx`) files in one or more Git Alternates.
2. Private packfiles (`*.pack`, `*.idx`, and `*.midx`) in `.git/objects/pack/`.
3. Loose objects (`.git/objects/XX/*`)

Packfiles use compression and delta-chains to reduce disk space (and network downloads), but it can be expensive to find and regenerate an individual object.

## Opportunities for Perf

Each Git command needs to access the ODB to lookup individual objects.  Conceptually, there are four steps required:

1. Scan the various shared and private packfile directories for `*.idx` and `*.midx` indexes and construct a list of in-memory dictionaries of object locations within packfiles.  (_While this list may be lazily created, it is still a startup cost in each Git process._)
2. When fetching an individual object, `mmap` packfile any fragments necessary to reconstruct the object. (_These fragments are mapped with generous address rounding on both ends, so we usually have good overlap with future object requests.  However, `mmap` isn't free and we do have to pay for page-faults.  And again, this memory mapping has to happen for each Git process._)
3. Within a packfile, an object is `zlib` compressed and it must be zlib-inflated to regenerate it.  This is expensive. (_When I was working on parallel checkout, I noticed that a significant percentage of the time was spent in zlib._)
4. Within a packfile, an object may be part of a delta-chain and may require a sequence of the [2] and [3] steps to regenerate the "data-base" before the current object may be regenerated.

Each of these steps gives us a "perf opportunity".

1. An ODB daemon could manage (preserve between clients) the list of index dictionaries.  (_Extra credit if it also watches the file system for created/deleted .idx files and automatically adapts._)
2. An ODB daemon could similarly manage the packfile memory mappings.
3. An ODB daemon could cache regenerated objects and respond without touching the disk, re-inflating, or re-de-delta-ing it.

## The State of the HACK

1. Create `git odb--daemon`
  1.1. This is a long-running thread pool-based service/daemon modeled on my FSMonitor daemon.
  1.2. It speaks IPC over Windows Named Pipes or Unix domain sockets.
  1.3. It caches all fetched objects and can re-serve them without touching the disk.

2. IPC upgrades
  2.1. Keep Alive: I upgraded the "Simple IPC" code used in FSMonitor to support multiple requests/responses on the same connection.  (FSMonitor only needed a simple single request/response model.)
  2.2. Binary Mode: I also upgraded it to allow binary transfers.

3. In all Git commands: I intercept all object lookups and route to the ODB daemon instead of directly touching the disk.

## Limitations of the HACK

_This is a hackathon project, so I did cut some corners._

1. On the client side, I intercepted all object lookups and route them to the ODB daemon and bypassed any client-side cacheing available.  A proper version would work with the existing in-process cache.

2. On the daemon side, I added a simple `oidmap` cache to remember and re-serve previously fetched objects.  No attempt was made to manage this cache -- it will just consume all memory in the daemon process.

3. On the daemon side, I do have a thread pool to service concurrent clients, but I didn't bother to make the `oidmap` cache thread safe or deal with any thread issues with in the existing ODB code.

4. The daemon is fairly limited and just responds to object requests.  There is no packfile management or pre-loading.

## Demo Results (using git.git)

|  | r1 | r2 | r3 | r4 | **avg** | r1 | r2 | r3 | r4 | **avg** | +/- |
| --- | -- | -- | -- | -- | ---         | -- | -- | -- | -- | --- | --- |
| [1] | 0.07 | 0.07 | 0.07 | 0.07 | **0.07**     | 0.08 | 0.08 | 0.08 | 0.10 | **0.085** | +0.015 |
| [2] | 0.07 | 0.07 | 0.07 | 0.07 | **0.07**     | 0.08 | 0.07 | 0.07 | 0.07 | **0.073** | +0.003 |
| [3] | 0.05 | 0.04 | 0.04 | 0.04 | **0.043** | 0.04 | 0.05 | 0.05 | 0.05 | **0.048** | +0.005 |
| [4] | 0.06 | 0.06 | 0.05 | 0.06 | **0.058**   | 0.07 | 0.07 | 0.06 | 0.06 | **0.065** | +0.007 |
| [5] | 7.93 | 7.85 | 7.92 | 8.07 | **7.943**      | 7.88 | 7.94 | 8.06 | 8.56 | **8.110** | +0.167 |
| [6] | 9.84 | 9.80 | 9.70 | 10.97 | **10.078** | 9.89 | 9.69 | 9.82 | 9.70 | **9.775** | -0.303 |

[1] `git status >/dev/null`
[2] `git diff HEAD~1 >/dev/null`
[3] `git log -200 >/dev/null`
[4] `git rev-list --objects HEAD^{tree} >/dev/null`
[5] `git rev-list --objects HEAD >/dev/null`
[6] `git rev-list --objects --all >/dev/null`

## Results

Perf-wise, the before and after numbers are about equal.  More testing is needed to see where time is being spent and where we can optimize, but this (relatively stupid) daemon demonstrates that an IPC-based solution is possible.

This lets us think about creating a smarter daemon to manage the ODB and access to it.
