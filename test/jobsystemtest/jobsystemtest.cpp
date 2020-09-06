
#include <assert.h>
#include <chrono>
#include <thread>

// jobsystem settings
#define JOBSYSTEM_ENABLE_PROFILING                ///< Enables worker/job profiling, and an ascii profile dump on shutdown.
#define JOBSYSTEM_ASSERT(...) assert(__VA_ARGS__) ///< Directs internal system asserts to app-specific assert mechanism.

// jobsystem include
#include <jobsystem.h>

int main()
{
    jobsystem::JobManagerDescriptor jobManagerDesc;
    
    const size_t kWorkerCount = 8;
    for (size_t i = 0; i < kWorkerCount; ++i)
    {
        jobManagerDesc.m_workers.emplace_back("Worker");
    }

    jobsystem::JobManager jobManager;
    if (!jobManager.Create(jobManagerDesc))
    {
        return 1;
    }

    auto something = []() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };

    auto somethingAfterThat = []() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };

    auto parallelThing1 = []() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };

    auto parallelThing2 = []() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };

    auto parallelThing3 = []() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };

    auto finalThing = []() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    };

    jobsystem::JobChainBuilder<10000> builder(jobManager);

    // Run a couple jobs in succession.
    builder
        .Do(something, 'a')
        .Then()
        .Do(somethingAfterThat, 'b')
        .Then()
        .Together();

    // Run 1k jobs in parallel.
    for (size_t i = 0; i < 1000; ++i)
    {
        const char c = 'a' + (char)(i % ('z' - 'a'));
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

    return builder.Failed() ? 1 : 0;
}
