## C++ Job System (Requires C++11 but nothing newer than that)

Implements a typical job system with features I've found useful in practice (Games, mostly).

Tested on Windows 10 and Ubuntu 18.

Features include:
- Simple interfaces for submitting, monitoring, and canceling jobs.
- Easy-to-configure affinities and policies to match workers to cores, as well as jobs to specific workers, which is useful for maximizing data-sharing and cache performance by constraining jobs operating in the same areas of memory to the same cluster(s).
- Basic work-stealing algorithm, enabling internal load-balancing between worker threads; currently lock-based.
- Full support for job dependency graphs.
- External thread-assist: Any thread outside of the worker pool can temporarily assist in job processing until some particular job is complete. A surprising percentage of production job systems lack this, leaving the producer thread to block/wait for jobs to be completed by the worker pool, which depending on your thread configuration can be a huge waste.
- Bare-bones but useful "profiler" showing a timeline for each worker with utilization over time. Jobs can be submitted with an optional "debug character", which allows one to visualize when specific jobs ran, for how long, and on which workers.
- Simple, straightforward, easy to modify. 
- No silly super modern C++, so does not rely on brand new compiler versions and potentially buggy STL implementations.
- MIT permissive license.

**Bug fixes, suggestions, complaints, and any knowledge I lack** is more than welcome. This isn't something I've iterated on heavily, and certainly wouldn't consider myself an expert in job system research.
