## Basic C++ Job System (Requires C++11)

Implements a typical job system with features I've found useful in practice (Games, mostly).

Features include:
- Simple interfaces for submitting, monitoring, and canceling jobs.
- Easy-to-configure affinities and policies for each worker.
- Basic work-stealing algorithm, enabling internal load-balancing between worker threads; currently lock-based.
- Full support for job dependency graphs.
- Ability to affinitize specific jobs to a subset of workers. Useful for maximizing data-sharing across cores sharing cache, and minimizing sharing across cores/modules that don't share cache. Job systems often suffer from cache-related performance problems on some platforms.
- External thread-assist: Any thread outside of the worker pool can temporarily assist in job processing until some particular job is complete. A surprising percentage of production job systems lack this, leaving the producer thread to block/wait for jobs to be completed by the worker pool, which depending on your thread configuration can be a huge waste.
- Bare-bones but useful "profiler" showing a timeline for each worker with utilization over time. Jobs can be submitted with an optional "debug character", which allows one to visualize when specific jobs ran, for how long, and on which workers.

Examples and unit tests exist, but I need to de-crud them before submitting.

**Bug fixes, suggestions, complaints, and any knowledge I obviously lack** is more than welcome. This isn't something I've iterated on heavily, and certainly wouldn't consider myself an expert. Please provide feedback!
