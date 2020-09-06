
#ifndef COMMONJOBSYSTEM_HEADER
#define COMMONJOBSYSTEM_HEADER

#pragma once

#if defined(WIN32) || defined(WIN64)
#   define WINDOWS
#elif defined(__unix__)
#   define LINUX
#endif

#ifdef WINDOWS
#   define NOMINMAX
#   define STRICT
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#endif

#include <algorithm>
#include <vector>
#include <deque>
#include <array>
#include <thread>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

namespace jobsystem
{
    inline uint64_t GetBit(uint64_t n) 
    { 
        return static_cast<uint64_t>(1) << n; 
    }

    inline size_t CountBits(uint64_t n) 
    { 
        size_t bits = 0; 
        while(n)
        {
            bits += n & 1;
            n >>= 1;
        }
        return bits;
    }

    typedef std::function<void()> JobDelegate;          ///< Structure of callbacks that can be requested as jobs.

    typedef uint64_t affinity_t;

    static const affinity_t kAffinityAllBits = static_cast<affinity_t>(~0);

    /**
     * Global system components.
     */
    std::atomic<size_t>             s_nextJobId;        ///< Job ID assignment for debugging / profiling.
    std::mutex                      s_signalLock;       ///< Global mutex for worker signaling.
    std::condition_variable         s_signalThreads;    ///< Global condition var for worker signaling.
    std::atomic<size_t>             s_activeWorkers;

    inline affinity_t CalculateSafeWorkerAffinity(size_t workerIndex, size_t workerCount)
    {
        affinity_t affinity = kAffinityAllBits; // Set all bits so jobs with affinities out of range can still be processed.
        affinity &= ~(workerCount - 1);         // Wipe bits within valid range.
        affinity |= GetBit(workerIndex);        // Set worker-specific bit.

        return affinity;
    }

    /**
     * Offers access to the state of job.
     * In particular, callers can use the Wait() function to ensure a given job is complete,
     * or Cancel().
     * Note, however, that doing so is not good practice with job systems. If no hardware threads
     * are available to process a given job, you can stall the caller for significant time.
     *
     * Internally, the state manages dependencies as well as atomics describing the status of the job.
     */
    typedef std::shared_ptr<class JobState> JobStatePtr;

    class JobState
    {
    private:

        friend class JobSystemWorker;
        friend class JobManager;

        std::atomic<bool>           m_cancel;           ///< Is the job pending cancellation?
        std::atomic<bool>           m_ready;            ///< Has the job been marked as ready for processing?

        std::vector<JobStatePtr>    m_dependants;       ///< List of dependent jobs.
        std::atomic<int>            m_dependencies;     ///< Number of outstanding dependencies.

        std::atomic<bool>           m_done;             ///< Has the job executed to completion?
        std::condition_variable     m_doneSignal;
        std::mutex                  m_doneMutex;

        affinity_t                  m_workerAffinity;   ///< Option to limit execution to specific worker threads / cores.

        size_t                      m_jobId;            ///< Debug/profiling ID.
        char                        m_debugChar;        ///< Debug character for profiling display.


        void SetQueued()
        {
            m_done.store(false, std::memory_order_release);
        }

        void SetDone()
        {
            JOBSYSTEM_ASSERT(!IsDone());

            for (const JobStatePtr& dependant : m_dependants)
            {
                dependant->m_dependencies.fetch_sub(1, std::memory_order_relaxed);
            }

            std::lock_guard<std::mutex> lock(m_doneMutex);
            m_done.store(true, std::memory_order_release);
            m_doneSignal.notify_all();
        }

        bool AwaitingCancellation() const
        {
            return m_cancel.load(std::memory_order_relaxed);
        }

    public:

        JobState()
            : m_debugChar(0)
        {
            m_jobId = s_nextJobId++;
            m_workerAffinity = kAffinityAllBits;

            m_dependencies.store(0, std::memory_order_release);
            m_cancel.store(false, std::memory_order_release);
            m_ready.store(false, std::memory_order_release);
            m_done.store(false, std::memory_order_release);
        }

        ~JobState() {}

        JobState& SetReady()
        {
            JOBSYSTEM_ASSERT(!IsDone());

            m_cancel.store(false, std::memory_order_relaxed);
            m_ready.store(true, std::memory_order_release);

            s_signalThreads.notify_all();

            return *this;
        }

        JobState& Cancel()
        {
            JOBSYSTEM_ASSERT(!IsDone());

            m_cancel.store(true, std::memory_order_relaxed);

            return *this;
        }

        JobState& AddDependant(JobStatePtr dependant)
        {
            JOBSYSTEM_ASSERT(m_dependants.end() == std::find(m_dependants.begin(), m_dependants.end(), dependant));

            m_dependants.push_back(dependant);

            dependant->m_dependencies.fetch_add(1, std::memory_order_relaxed);

            return *this;
        }

        JobState& SetWorkerAffinity(affinity_t affinity)
        {
            m_workerAffinity = affinity ? affinity : kAffinityAllBits;

            return *this;
        }

        bool IsDone() const
        {
            return m_done.load(std::memory_order_acquire);
        }

        bool Wait(size_t maxWaitMicroseconds = 0)
        {
            if (!IsDone())
            {
                std::unique_lock<std::mutex> lock(m_doneMutex);

                if (maxWaitMicroseconds == 0)
                {
                    m_doneSignal.wait(lock,
                        [this]()
                        {
                            return IsDone();
                        }
                    );
                }
                else
                {
                    m_doneSignal.wait_for(lock, std::chrono::microseconds(maxWaitMicroseconds));
                }
            }

            return IsDone();
        }

        bool AreDependenciesMet() const
        {
            if (!m_ready.load(std::memory_order_acquire))
            {
                return false;
            }

            if (m_dependencies.load(std::memory_order_relaxed) > 0)
            {
                return false;
            }

            return true;
        }

        bool HasDependencies() const
        {
            return (m_dependencies.load(std::memory_order_relaxed) > 0);
        }
    };

    /**
     * Represents an entry in a job queue.
     * - A delegate to invoke
     * - Internal job state
     */
    struct JobQueueEntry
    {
        JobDelegate     m_delegate;     ///< Delegate to invoke for the job.
        JobStatePtr     m_state;        ///< Pointer to job state.
    };

    /**
     * Descriptor for a given job worker thread, to be provided by the host application.
     */
    struct JobWorkerDescriptor
    {
        JobWorkerDescriptor(const char* name = "JobSystemWorker", affinity_t cpuAffinity = affinity_t(~0), bool enableWorkSteeling = true)
            : m_name(name)
            , m_cpuAffinity(cpuAffinity)
            , m_enableWorkStealing(enableWorkSteeling)
        {
        }

        std::string     m_name;                     ///< Worker name, for debug/profiling displays.
        affinity_t      m_cpuAffinity;              ///< Thread affinity. Defaults to all cores.
        bool            m_enableWorkStealing : 1;   ///< Enable queue-sharing between workers?
    };

    /**
     * Job events (for tracking/debugging).
     */
    enum EJobEvent
    {
        eJobEvent_JobPopped,            ///< A job was popped from a queue.
        eJobEvent_JobStart,             ///< A job is about to start.
        eJobEvent_JobDone,              ///< A job just completed.
        eJobEvent_JobRun,               ///< A job has been completed.
        eJobEvent_JobRunAssisted,       ///< A job has been completed through outside assistance.
        eJobEvent_JobStolen,            ///< A worker has stolen a job from another worker. 
        eJobEvent_WorkerAwoken,         ///< A worker has been awoken.
        eJobEvent_WorkerUsed,           ///< A worker has been utilized.
    };

    typedef std::function<void(const JobQueueEntry& job, EJobEvent, uint64_t, size_t)> JobEventObserver;  ///< Delegate definition for job event observation.

    typedef std::deque<JobQueueEntry> JobQueue;     ///< Data structure to represent job queue.

    /**
     * High-res clock based on windows performance counter. Supports STL chrono interfaces.
     */
    using TimePoint = std::chrono::high_resolution_clock::time_point;
    TimePoint ProfileClockNow()
    {
        return std::chrono::high_resolution_clock::now();
    }

    /**
     * Tracking each job's start/end times in a per-worker timeline, for debugging/profiling.
     */
    class ProfilingTimeline
    {
    public:

        struct TimelineEntry
        {
            uint64_t                        jobId;                  ///< ID of the job that generated this timeline entry.
            TimePoint                       start;                  ///< Job start time.
            TimePoint                       end;	                ///< Job end time.
            char                            debugChar;              ///< Job's debug character for profiling display.

            std::string                     description;            ///< Timeline entry description.
        };

        typedef std::vector<TimelineEntry> TimelineEntries;

        TimelineEntries                     m_entries;              //< List of timeline entries for this thread.
    };

    /**
     * Represents a worker thread.
     * - Owns a job queue
     * - Implements work-stealing from other workers
     */
    class JobSystemWorker
    {
        friend class JobManager;

    public:

        JobSystemWorker(const JobWorkerDescriptor& desc, const JobEventObserver& eventObserver)
            : m_allWorkers(nullptr)
            , m_workerCount(0)
            , m_workerIndex(0)
            , m_desc(desc)
            , m_eventObserver(eventObserver)
            , m_hasShutDown(false)
        {
        }

        void Start(size_t index, JobSystemWorker** allWorkers, size_t workerCount)
        {
            m_allWorkers = allWorkers;
            m_workerCount = workerCount;
            m_workerIndex = index;

            m_thread = std::thread(&JobSystemWorker::WorkerThreadProc, this);
        }

        void Shutdown()
        {
            m_stop.store(true, std::memory_order_relaxed);

            while (!m_hasShutDown.load(std::memory_order_acquire))
            {
                s_signalThreads.notify_all();

                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }

            if (m_hasShutDown.load(std::memory_order_acquire))
            {
                m_thread.join();
            }
        }

        JobStatePtr PushJob(JobDelegate delegate)
        {
            JobQueueEntry entry = { delegate, std::make_shared<JobState>() };
            entry.m_state->SetQueued();

            {
                std::lock_guard<std::mutex> queueLock(m_queueLock);
                m_queue.insert(m_queue.begin(), entry);
            }

            return entry.m_state;
        }

    private:

        void NotifyEventObserver(const JobQueueEntry& job, EJobEvent event, uint64_t workerIndex, size_t jobId = 0)
        {
#ifdef JOBSYSTEM_ENABLE_PROFILING

            if (m_eventObserver)
            {
                m_eventObserver(job, event, workerIndex, jobId);
            }

#endif // JOBSYSTEM_ENABLE_PROFILING
        }

        bool PopJobFromQueue(JobQueue& queue, JobQueueEntry& job, bool& hasUnsatisfiedDependencies, affinity_t workerAffinity)
        {
            for (auto jobIter = queue.begin(); jobIter != queue.end();)
            {
                const JobQueueEntry& candidate = (*jobIter);

                if ((workerAffinity & candidate.m_state->m_workerAffinity) != 0)
                {
                    if (candidate.m_state->AwaitingCancellation())
                    {
                        candidate.m_state->SetDone();
                        jobIter = queue.erase(jobIter);

                        continue;
                    }
                    else if (candidate.m_state->AreDependenciesMet())
                    {
                        job = candidate;
                        queue.erase(jobIter);

                        NotifyEventObserver(job, eJobEvent_JobPopped, m_workerIndex);

                        return true;
                    }
                }

                hasUnsatisfiedDependencies = true;
                ++jobIter;
            }

            return false;
        }

        bool PopNextJob(JobQueueEntry& job, bool& hasUnsatisfiedDependencies, bool useWorkStealing, affinity_t workerAffinity)
        {
            bool foundJob = false;

            {
                std::lock_guard<std::mutex> queueLock(m_queueLock);
                foundJob = PopJobFromQueue(m_queue, job, hasUnsatisfiedDependencies, workerAffinity);
            }

            if (!foundJob && useWorkStealing)
            {
                for (size_t i = 0; foundJob == false && i < m_workerCount; ++i)
                {
                    JOBSYSTEM_ASSERT(m_allWorkers[i]);
                    JobSystemWorker& worker = *m_allWorkers[i];

                    {
                        std::lock_guard<std::mutex> queueLock(worker.m_queueLock);
                        foundJob = PopJobFromQueue(worker.m_queue, job, hasUnsatisfiedDependencies, workerAffinity);
                    }
                }

                if (foundJob)
                {
                    NotifyEventObserver(job, eJobEvent_JobStolen, m_workerIndex);
                }
            }

            return foundJob;
        }

        void SetThreadName(const char* name)
        {
            (void)name;
#if defined(WINDOWS)
            typedef struct tagTHREADNAME_INFO
            {
                unsigned long dwType; // must be 0x1000
                const char* szName; // pointer to name (in user addr space)
                unsigned long dwThreadID; // thread ID (-1=caller thread)
                unsigned long dwFlags; // reserved for future use, must be zero
            } THREADNAME_INFO;

            THREADNAME_INFO threadName;
            threadName.dwType = 0x1000;
            threadName.szName = name;
            threadName.dwThreadID = GetCurrentThreadId();
            threadName.dwFlags = 0;
            __try
            {
                RaiseException(0x406D1388, 0, sizeof(threadName) / sizeof(ULONG_PTR), (ULONG_PTR*)&threadName);
            }
            __except (EXCEPTION_CONTINUE_EXECUTION)
            {
            }
#elif defined(LINUX)
            pthread_setname_np(pthread_self(), name);
#endif
        }

        void WorkerThreadProc()
        {
            SetThreadName(m_desc.m_name.c_str());

#if defined(WINDOWS)
            SetThreadAffinityMask(m_thread.native_handle(), m_desc.m_cpuAffinity);
#elif defined(LINUX)
            cpu_set_t cpuset; 
            CPU_ZERO(&cpuset);
            for(size_t i = 0; i < sizeof(m_desc.m_cpuAffinity) * 8; ++i)
            {
                if ((1 << i) & m_desc.m_cpuAffinity)
                {
                    CPU_SET(i, &cpuset);
                }
            }
#endif

            const affinity_t workerAffinity = CalculateSafeWorkerAffinity(m_workerIndex, m_workerCount);

            while (true)
            {
                JobQueueEntry job;
                {
                    std::unique_lock<std::mutex> signalLock(s_signalLock);

                    bool hasUnsatisfiedDependencies;

                    while (!m_stop.load(std::memory_order_relaxed) &&
                           !PopNextJob(job, hasUnsatisfiedDependencies, m_desc.m_enableWorkStealing, workerAffinity))
                    {
                        s_signalThreads.wait(signalLock);
                        NotifyEventObserver(job, eJobEvent_WorkerAwoken, m_workerIndex);
                    }
                }

                if (m_stop.load(std::memory_order_relaxed))
                {
                    m_hasShutDown.store(true, std::memory_order_release);

                    break;
                }

                s_activeWorkers.fetch_add(1, std::memory_order_acq_rel);
                {
                    NotifyEventObserver(job, eJobEvent_WorkerUsed, m_workerIndex);

                    NotifyEventObserver(job, eJobEvent_JobStart, m_workerIndex, job.m_state->m_jobId);
                    job.m_delegate();
                    NotifyEventObserver(job, eJobEvent_JobDone, m_workerIndex);

                    job.m_state->SetDone();

                    NotifyEventObserver(job, eJobEvent_JobRun, m_workerIndex);

                    s_signalThreads.notify_one();
                }
                s_activeWorkers.fetch_sub(1, std::memory_order_acq_rel);
            }
        }

        std::thread                 m_thread;                   ///< Thread instance for worker.
        std::atomic<bool>           m_stop;                     ///< Has a stop been requested?
        std::atomic<bool>           m_hasShutDown;              ///< Has the worker completed shutting down?

        mutable std::mutex          m_queueLock;                ///< Mutex to guard worker queue.
        JobQueue                    m_queue;                    ///< Queue containing requested jobs.

        JobSystemWorker**           m_allWorkers;               ///< Pointer to array of all workers, for queue-sharing / work-stealing.
        size_t                      m_workerCount;              ///< Number of total workers (size of m_allWorkers array).
        size_t                      m_workerIndex;              ///< This worker's index within m_allWorkers.

        JobEventObserver            m_eventObserver;            ///< Observer of job-related events occurring on this worker.
        JobWorkerDescriptor         m_desc;                     ///< Descriptor/configuration of this worker.
    };

    /**
     * Descriptor for configuring the job manager.
     * - Contains descriptor for each worker
     */
    struct JobManagerDescriptor
    {
        std::vector<JobWorkerDescriptor> m_workers;             ///< Configurations for all workers that should be spawned by JobManager.
    };

    /**
     * Manages job workers, and acts as the primary interface to the job queue.
     */
    class JobManager
    {
    private:

        void Observer(const JobQueueEntry& job, EJobEvent event, uint64_t workerIndex, size_t jobId = 0)
        {
#ifdef JOBSYSTEM_ENABLE_PROFILING
            switch (event)
            {
            case eJobEvent_JobRun:
            {
                ++m_jobsRun;
            }
            break;

            case eJobEvent_JobStolen:
            {
                ++m_jobsStolen;
            }
            break;

            case eJobEvent_JobRunAssisted:
            {
                ++m_jobsAssisted;
                ++m_jobsRun;
            }
            break;

            case eJobEvent_WorkerAwoken:
            {
                m_awokenMask |= GetBit(workerIndex);
            }
            break;

            case eJobEvent_WorkerUsed:
            {
                m_usedMask |= GetBit(workerIndex);
            }
            break;

            case eJobEvent_JobStart:
            {
                ProfilingTimeline& timeline = workerIndex < m_workers.size() ? m_timelines[workerIndex] : m_timelines[m_workers.size()];
                ProfilingTimeline::TimelineEntry entry;
                entry.jobId = jobId;
                entry.start = ProfileClockNow();
                entry.debugChar = job.m_state ? job.m_state->m_debugChar : 0;
                timeline.m_entries.push_back(entry);
            }
            break;

            case eJobEvent_JobDone:
            {
                ProfilingTimeline& timeline = workerIndex < m_workers.size() ? m_timelines[workerIndex] : m_timelines[m_workers.size()];
                ProfilingTimeline::TimelineEntry& entry = timeline.m_entries.back();
                entry.end = ProfileClockNow();
            }
            break;

            case eJobEvent_JobPopped:
            {
                if (!m_hasPushedJob)
                {
                    m_firstJobTime = ProfileClockNow();
                    m_hasPushedJob = true;
                }
            }
            break;
            }
#endif // JOBSYSTEM_ENABLE_PROFILING
        }

    public:

        JobManager()
            : m_jobsRun(0)
            , m_jobsStolen(0)
            , m_usedMask(0)
            , m_awokenMask(0)
            , m_nextRoundRobinWorkerIndex(0)
            , m_timelines(nullptr)
            , m_firstJobTime()
        {

        }

        ~JobManager()
        {
            DumpProfilingResults();

            JoinWorkersAndShutdown();
        }

        bool Create(const JobManagerDescriptor& desc)
        {
            JoinWorkersAndShutdown();

            m_desc = desc;

            const size_t workerCount = desc.m_workers.size();
            m_workers.reserve(workerCount);

#ifdef JOBSYSTEM_ENABLE_PROFILING

            m_timelines = new ProfilingTimeline[workerCount + 1];
            m_hasPushedJob = false;

#endif // JOBSYSTEM_ENABLE_PROFILING

            const JobEventObserver observer = std::bind(
                &JobManager::Observer, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4);

            // Create workers. We don't spawn the threads yet.
            for (size_t i = 0; i < workerCount; ++i)
            {
                const JobWorkerDescriptor& workerDesc = desc.m_workers[i];

                JobSystemWorker* worker = new JobSystemWorker(workerDesc, observer);
                m_workers.push_back(worker);
            }

            // Start the workers (includes spawning threads). Each worker maintains
            // understanding of what other workers exist, for work-stealing purposes.
            for (size_t i = 0; i < workerCount; ++i)
            {
                m_workers[i]->Start(i, &m_workers[0], workerCount);
            }

            return !m_workers.empty();
        }

        JobStatePtr AddJob(JobDelegate delegate, char debugChar = 0)
        {
            JobStatePtr state = nullptr;

            // \todo - workers should maintain a tls pointer to themselves, so we can push
            // directly into our own queue.

            if (!m_workers.empty())
            {
                // Add round-robin style. Note that work-stealing helps load-balance,
                // if it hasn't been disabled. If it has we may need to consider a
                // smarter scheme here.
                state = m_workers[m_nextRoundRobinWorkerIndex]->PushJob(delegate);
                state->m_debugChar = debugChar;

                m_nextRoundRobinWorkerIndex = (m_nextRoundRobinWorkerIndex + 1) % m_workers.size();
            }

            return state;
        }

        void AssistUntilJobDone(JobStatePtr state)
        {
            JOBSYSTEM_ASSERT(state->m_ready.load(std::memory_order_acquire));

            const affinity_t workerAffinity = kAffinityAllBits;

            // Steal jobs from workers until the specified job is done.
            while (!state->IsDone())
            {
                JOBSYSTEM_ASSERT(!m_workers.empty());

                JobQueueEntry job;
                bool hasUnsatisfiedDependencies;

                if (m_workers[0]->PopNextJob(job, hasUnsatisfiedDependencies, true, workerAffinity))
                {
                    Observer(job, eJobEvent_JobStart, m_workers.size(), job.m_state->m_jobId);
                    job.m_delegate();
                    Observer(job, eJobEvent_JobDone, m_workers.size());

                    job.m_state->SetDone();

                    Observer(job, eJobEvent_JobRunAssisted, 0);

                    s_signalThreads.notify_one();
                }
            }
        }

        void AssistUntilDone()
        {
            JOBSYSTEM_ASSERT(!m_workers.empty());

            // Steal and run jobs from workers until all queues are exhausted.

            const affinity_t workerAffinity = kAffinityAllBits;

            JobQueueEntry job;
            bool foundBusyWorker = true;

            while (foundBusyWorker)
            {
                foundBusyWorker = false;

                for (JobSystemWorker* worker : m_workers)
                {
                    if (worker->PopNextJob(job, foundBusyWorker, false, workerAffinity))
                    {
                        Observer(job, eJobEvent_JobStart, m_workers.size(), job.m_state->m_jobId);
                        job.m_delegate();
                        Observer(job, eJobEvent_JobDone, m_workers.size());

                        job.m_state->SetDone();

                        Observer(job, eJobEvent_JobRunAssisted, 0);

                        foundBusyWorker = true;
                        s_signalThreads.notify_one();
                        break;
                    }
                }
            }

            for (JobSystemWorker* worker : m_workers)
            {
                if (!worker->m_queue.empty())
                {
                    JOBSYSTEM_ASSERT(0);
                }
            }
        }

        void JoinWorkersAndShutdown(bool finishJobs = false)
        {
            if (finishJobs)
            {
                AssistUntilDone();
            }

            // Tear down each worker. Un-popped jobs may still reside in the queues at this point
            // if finishJobs = false.
            // Don't destruct workers yet, in case someone's in the process of work-stealing.
            for (size_t i = 0, n = m_workers.size(); i < n; ++i)
            {
                JOBSYSTEM_ASSERT(m_workers[i]);
                m_workers[i]->Shutdown();
            }

            // Destruct all workers.
            std::for_each(m_workers.begin(), m_workers.end(), [](JobSystemWorker* worker) { delete worker; });
            m_workers.clear();

#ifdef JOBSYSTEM_ENABLE_PROFILING

            delete[] m_timelines;
            m_timelines = nullptr;

#endif // JOBSYSTEM_ENABLE_PROFILING
        }

    private:

        size_t                          m_nextRoundRobinWorkerIndex;    ///< Index of the worker to receive the next requested job, round-robin style.

        std::atomic<unsigned int>       m_jobsRun;                      ///< Counter to track # of jobs run.
        std::atomic<unsigned int>       m_jobsAssisted;                 ///< Counter to track # of jobs run via external Assist*().
        std::atomic<unsigned int>       m_jobsStolen;                   ///< Counter to track # of jobs stolen from another worker's queue.
        std::atomic<affinity_t>         m_usedMask;                     ///< Mask with bits set according to the IDs of the jobs that have executed jobs.
        std::atomic<affinity_t>         m_awokenMask;                   ///< Mask with bits set according to the IDs of the jobs that have been awoken at least once.

    private:

        JobManagerDescriptor             m_desc;                         ///< Descriptor/configuration of the job manager.

        bool                            m_hasPushedJob;                 ///< For profiling - has a job been pushed yet?
        TimePoint                       m_firstJobTime;                 ///< For profiling - when was the first job pushed?
        ProfilingTimeline*              m_timelines;                    ///< For profiling - a ProfilingTimeline entry for each worker, plus an additional entry to represent the Assist thread.

        std::vector<JobSystemWorker*>   m_workers;                      ///< Storage for worker instances.

        void DumpProfilingResults()
        {
#ifdef JOBSYSTEM_ENABLE_PROFILING

            AssistUntilDone();

            auto now = ProfileClockNow();
            auto totalNS = std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_firstJobTime).count();

            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            const size_t workerCount = m_workers.size();

            printf(
                "\n[Job System Statistics]\n"
                "Jobs Run:       %8d\n" // May be < jobs submitted
                "Jobs Stolen:    %8d\n"
                "Jobs Assisted:  %8d\n"
                "Workers Used:   %8lu\n"
                "Workers Awoken: %8lu\n"
                ,
                m_jobsRun.load(std::memory_order_acquire),
                m_jobsStolen.load(std::memory_order_acquire),
                m_jobsAssisted.load(std::memory_order_acquire),
                CountBits(m_usedMask.load(std::memory_order_acquire)),
                CountBits(m_awokenMask.load(std::memory_order_acquire)));

            printf("\n[Worker Profiling Results]\n%.3f total ms\n\nTimeline (approximated):\n\n", double(totalNS) / 1000000);

            const char* busySymbols = "abcdefghijklmn";
            const size_t busySymbolCount = strlen(busySymbols);

            for (size_t workerIndex = 0; workerIndex < workerCount + 1; ++workerIndex)
            {
                ProfilingTimeline& timeline = m_timelines[workerIndex];

                const char* name = (workerIndex < workerCount) ? m_workers[workerIndex]->m_desc.m_name.c_str() : "[Assist]";

                const size_t bufferSize = 200;
                char buffer[bufferSize];
                snprintf(buffer, sizeof(buffer), "%20s: ", name);

                const size_t nameLen = strlen(buffer);
                const size_t remaining = bufferSize - nameLen - 2;

                for (size_t nameIdx = nameLen; nameIdx < bufferSize - 2; ++nameIdx)
                {
                    buffer[nameIdx] = '-';
                }

                buffer[bufferSize - 2] = '\n';
                buffer[bufferSize - 1] = 0;

                for (ProfilingTimeline::TimelineEntry& entry : timeline.m_entries)
                {
                    const auto startNs = std::chrono::duration_cast<std::chrono::nanoseconds>(entry.start - m_firstJobTime).count();
                    const auto endNs = std::chrono::duration_cast<std::chrono::nanoseconds>(entry.end - m_firstJobTime).count();

                    const double startPercent = (double(startNs) / double(totalNS));
                    const double endPercent = (double(endNs) / double(totalNS));

                    const char jobCharacter = (entry.debugChar != 0) ? entry.debugChar : busySymbols[entry.jobId % busySymbolCount];

                    const size_t startIndex = nameLen + std::min<size_t>(remaining - 1, size_t(startPercent * double(remaining)));
                    size_t endIndex = nameLen + std::min<size_t>(remaining - 1, size_t(endPercent * double(remaining)));

                    size_t shift = 0;

                    while (buffer[startIndex + shift] != '-' && startIndex + shift < bufferSize - 3 && endIndex + shift < bufferSize - 3)
                    {
                        ++shift;
                    }

                    endIndex -= std::min<size_t>(endIndex - startIndex, size_t(shift));

                    for (size_t i = startIndex + shift; i <= endIndex + shift; ++i)
                    {
                        JOBSYSTEM_ASSERT(i < bufferSize - 2);
                        buffer[i] = jobCharacter;
                    }
                }

                printf("%s", buffer);
            }

            printf("\n");

#endif // JOBSYSTEM_ENABLE_PROFILING
        }
    };

    /**
     * Helper for building complex job/dependency chains logically.
     *
     * e.g.
     *
     * jobsystem::JobManager jobManager;
     * ...
     * jobsystem::JobChainBuilder<128>(jobManager)
     *  .Do(something, 'a')
     *  .Then()
     *  .Do(somethingAfterThat, 'b')
     *  .Then()
     *  .Together()
     *      .Do(parallelThing1, 'c')
     *      .Do(parallelThing2, 'd')
     *      .Do(parallelThing3, 'e')
     *  .Close()
     *  .Then()
     *  .Do(finalThing, 'F')
     *  .Go()
     * .WaitForAll();
     **                                    --- parallelThing1 ---
     *                                   /                       \
     *  something -> somethingAfterThat -> --- parallelThing2 ---- -> finalThing
     *                                   \                       /
     *                                     --- parallelThing3 ---
     * etc...
     *
     */
    template<size_t MaxJobNodes = 256>
    class JobChainBuilder
    {
    public:

        struct Node
        {
            Node() : isGroup(false), groupDependency(nullptr) {}
            ~Node() {}

            Node*			groupDependency;
            JobStatePtr		job;
            bool			isGroup;
        };

        Node* AllocNode()
        {
            if (m_nextNodeIndex >= MaxJobNodes)
                return nullptr;

            Node* node = &m_nodePool[m_nextNodeIndex++];
            *node = Node();

            return node;
        }

        JobChainBuilder(JobManager& manager)
            : mgr(manager)
        {
            Reset();

            // Push a sentinel (root) node.
            m_stack.push_back(AllocNode());
        }

        void Reset()
        {
            m_allJobs.clear();
            m_stack.clear();

            m_last = nullptr;
            m_dependency = nullptr;
            m_nextNodeIndex = 0;
            m_failed = false;
        }

        JobChainBuilder& Together(char debugChar = 0)
        {
            if (Node* item = AllocNode())
            {
                item->isGroup = true;
                item->groupDependency = m_dependency;

                item->job = mgr.AddJob([]() {}, debugChar);

                m_allJobs.push_back(item->job);

                m_last = item;
                m_dependency = nullptr;

                m_stack.push_back(item);
            }
            else
            {
                Fail();
            }

            return *this;
        }

        JobChainBuilder& Do(JobDelegate delegate, char debugChar = 0)
        {
            Node* owner = m_stack.back();

            if (Node* item = AllocNode())
            {
                item->job = mgr.AddJob(delegate, debugChar);

                m_allJobs.push_back(item->job);

                if (m_dependency)
                {
                    m_dependency->job->AddDependant(item->job);
                    m_dependency = nullptr;
                }

                if (owner && owner->isGroup)
                {
                    item->job->AddDependant(owner->job);

                    if (owner->groupDependency)
                    {
                        owner->groupDependency->job->AddDependant(item->job);
                    }
                }

                m_last = item;
            }
            else
            {
                Fail();
            }

            return *this;
        }

        JobChainBuilder& Then()
        {
            m_dependency = m_last;
            m_last = (m_dependency) ? m_dependency->groupDependency : nullptr;

            return *this;
        }

        JobChainBuilder& Close()
        {
            if (!m_stack.empty())
            {
                Node* owner = m_stack.back();
                if (owner->isGroup)
                {
                    m_last = owner;
                }
            }

            m_dependency = nullptr;

            if (m_stack.size() > 1)
            {
                m_stack.pop_back();
            }

            return *this;
        }

        JobChainBuilder& Go()
        {
            if (m_allJobs.empty())
            {
                return *this;
            }

            Then();
            Do([]() {}, 'J');
            m_joinJob = m_allJobs.back();

            for (JobStatePtr& job : m_allJobs)
            {
                job->SetReady();
            }

            return *this;
        }

        void Fail()
        {
            for (JobStatePtr& job : m_allJobs)
            {
                job->Cancel();
            }

            m_allJobs.clear();
            m_failed = true;
        }

        bool Failed() const
        {
            return m_failed;
        }

        void WaitForAll()
        {
            if (m_joinJob)
            {
                m_joinJob->Wait();
            }
        }

        void AssistAndWaitForAll()
        {
            mgr.AssistUntilJobDone(m_joinJob);
        }

        JobManager&                 mgr;                        ///< Job manager to submit jobs to.

        Node                        m_nodePool[MaxJobNodes];    ///< Pool of chain nodes (on the stack). The only necessary output of this system is jobs. Nodes are purely internal.
        size_t                      m_nextNodeIndex;            ///< Next free item in the pool.

        std::vector<Node*>          m_stack;                    ///< Internal stack to track groupings.
        std::vector<JobStatePtr>    m_allJobs;                  ///< All jobs created by the builder, to be readied on completion.

        Node*                       m_last;                     ///< Last job to be pushed, to handle setting up dependencies after Then() calls.
        Node*                       m_dependency;               ///< Any job promoted to a dependency for the next job, as dicated by Then().

        JobStatePtr                 m_joinJob;                  ///< Final join job that callers can wait on to complete the batch.

        bool                        m_failed;                   ///< Did an error occur during creation of the DAG?
    };


} // namespace jobsystem

#endif // COMMONJOBSYSTEM_HEADER
