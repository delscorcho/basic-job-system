
#include <assert.h>
#include <chrono>
#include <thread>

// jobsystem settings
#define JOBSYSTEM_ENABLE_PROFILING                ///< Enables worker/job profiling, and an ascii profile dump on shutdown.
#define JOBSYSTEM_ASSERT(...) assert(__VA_ARGS__) ///< Directs internal system asserts to app-specific assert mechanism.

// jobsystem include
#include <jobsystem.h>

#include "dex_profiling/ICapture.h"
#include "dex_profiling/ICapture.cpp"

int main()
{
    jobsystem::JobManagerDescriptor jobManagerDesc;
    
    const size_t kWorkerCount = 16;
    for (size_t i = 0; i < kWorkerCount; ++i)
    {
        jobManagerDesc.m_workers.emplace_back("Worker");
    }

    jobsystem::JobManager jobManager;
    if (!jobManager.Create(jobManagerDesc))
    {
        return 1;
    }

    const size_t kNumParallelJobs = 1000;
    const size_t kItersPerJob = 100000;

    float floats[64];

    auto something = [&]() {
        DEX_ICAPTURE_ZONE(~0, 0, "something");
        for (size_t i = 0; i < kItersPerJob; ++i)
            floats[0] *= 5.f;
    };

    auto somethingAfterThat = [&]() {
        DEX_ICAPTURE_ZONE(~0, 0, "somethingAfterThat");
        for (size_t i = 0; i < kItersPerJob; ++i)
            floats[8] *= 5.f;
    };

    auto parallelThing1 = [&]() {
        DEX_ICAPTURE_ZONE(~0, 0, "parallelThing1");
        for (size_t i = 0; i < kItersPerJob; ++i)
            floats[16] *= 5.f;
    };

    auto parallelThing2 = [&]() {
        DEX_ICAPTURE_ZONE(~0, 0, "parallelThing2");
        for (size_t i = 0; i < kItersPerJob; ++i)
            floats[24] *= 5.f;
    };

    auto parallelThing3 = [&]() {
        DEX_ICAPTURE_ZONE(~0, 0, "parallelThing3");
        for (size_t i = 0; i < kItersPerJob; ++i)
            floats[32] *= 5.f;
    };

    auto finalThing = [&]() {
        DEX_ICAPTURE_ZONE(~0, 0, "finalThing");
        for (size_t i = 0; i < kItersPerJob; ++i)
            floats[40] *= 5.f;
    };

    dex::ICaptureStart("./capture_1_proc.icap");

    jobsystem::JobChainBuilder<10000> builder(jobManager);

    // Run a couple jobs in succession.
    builder
        .Do(something, 'a')
        .Then()
        .Do(somethingAfterThat, 'b')
        .Then()

    // Run 1k jobs in parallel.
    .Together();
    for (size_t i = 0; i < kNumParallelJobs; ++i)
    {
        const char c = 'A' + (char)(i % ('z' - 'A'));
        builder.Do(parallelThing1, c);
    }

    // Run a final "join" job.
    builder
        .Close()
        .Then()
        .Do(finalThing, 'Z');

    // Run the jobs and assist until complete.
    builder
        .Go()
        .AssistAndWaitForAll();

    dex::ICaptureStop();

    return builder.Failed() ? 1 : 0;
}
