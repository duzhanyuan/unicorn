
/**
 * Copyright (c) 2011 Indian Institute of Technology, New Delhi
 * All Rights Reserved
 *
 * Entire information in this file and PMLIB software is property
 * of Indian Institue of Technology, New Delhi. Redistribution, 
 * modification and any use in source form is strictly prohibited
 * without formal written approval from Indian Institute of Technology, 
 * New Delhi. Use of software in binary form is allowed provided
 * the using application clearly highlights the credits.
 *
 * This work is the doctoral project of Tarun Beri under the guidance
 * of Prof. Subodh Kumar and Prof. Sorav Bansal. More information
 * about the authors is available at their websites -
 * Prof. Subodh Kumar - http://www.cse.iitd.ernet.in/~subodh/
 * Prof. Sorav Bansal - http://www.cse.iitd.ernet.in/~sbansal/
 * Tarun Beri - http://www.cse.iitd.ernet.in/~tarun
 */

#ifndef __PM_EXECUTION_STUB__
#define __PM_EXECUTION_STUB__

#include "pmBase.h"
#include "pmThread.h"

#ifdef DUMP_EVENT_TIMELINE
    #include "pmEventTimeline.h"
#endif

#ifdef SUPPORT_CUDA
#include "pmMemChunk.h"
#endif

#include <setjmp.h>
#include <string.h>

namespace pm
{

class pmTask;
class pmProcessingElement;
class pmSubscriptionManager;
class pmReducer;

/**
 * \brief The controlling thread of each processing element.
 */

namespace execStub
{

typedef enum eventIdentifier
{
    THREAD_BIND
    , SUBTASK_EXEC
    , SUBTASK_REDUCE
    , NEGOTIATED_RANGE
	, FREE_GPU_RESOURCES
    , POST_HANDLE_EXEC_COMPLETION
    , DEFERRED_SHADOW_MEM_COMMITS
    , REDUCTION_FINISH
    , PROCESS_REDISTRIBUTION_BUCKET
    , FREE_TASK_RESOURCES
#ifdef DUMP_EVENT_TIMELINE
    , INIT_EVENT_TIMELINE
#endif
} eventIdentifier;

typedef struct threadBind
{
    size_t physicalMemory;
    size_t totalStubCount;
} threadBind;

typedef struct subtaskExec
{
    pmSubtaskRange range;
    bool rangeExecutedOnce;
    ulong lastExecutedSubtaskId;
} subtaskExec;

typedef struct subtaskReduce
{
    pmTask* task;
    ulong subtaskId1;
    pmExecutionStub* stub2;
    ulong subtaskId2;
} subtaskReduce;

typedef struct negotiatedRange
{
    pmSubtaskRange range;
} negotiatedRange;
    
typedef struct execCompletion
{
    pmSubtaskRange range;
    pmStatus execStatus;
} execCompletion;
    
typedef struct deferredShadowMemCommits
{
    pmTask* task;
} deferredShadowMemCommits;
    
typedef struct reductionFinish
{
    pmTask* task;
} reductionFinish;
    
typedef struct processRedistributionBucket
{
    pmTask* task;
    size_t bucketIndex;
} processRedistributionBucket;
    
typedef struct freeTaskResources
{
    pmMachine* taskOriginatingHost;
    ulong taskSequenceNumber;
} freeTaskResources;
    
#ifdef DUMP_EVENT_TIMELINE
typedef struct initTimeline
{
} initTimeline;
#endif

typedef struct stubEvent : public pmBasicThreadEvent
{
    eventIdentifier eventId;
    union
    {
        threadBind bindDetails;
        subtaskExec execDetails;
        subtaskReduce reduceDetails;
        negotiatedRange negotiatedRangeDetails;
        execCompletion execCompletionDetails;
        deferredShadowMemCommits deferredShadowMemCommitsDetails;
        reductionFinish reductionFinishDetails;
        processRedistributionBucket processRedistributionBucketDetails;
        freeTaskResources freeTaskResourcesDetails;
    #ifdef DUMP_EVENT_TIMELINE
        initTimeline initTimelineDetails;
    #endif
    };

    virtual bool BlocksSecondaryCommands();
} stubEvent;

}

class pmExecutionStub : public THREADING_IMPLEMENTATION_CLASS<execStub::stubEvent>
{
	public:
		pmExecutionStub(uint pDeviceIndexOnMachine);
		virtual ~pmExecutionStub();

		virtual pmStatus BindToProcessingElement() = 0;

		virtual pmStatus Push(pmSubtaskRange& pRange);
		virtual pmStatus ThreadSwitchCallback(execStub::stubEvent& pEvent);

		virtual std::string GetDeviceName() = 0;
		virtual std::string GetDeviceDescription() = 0;

		virtual pmDeviceType GetType() = 0;

		pmProcessingElement* GetProcessingElement();

        pmStatus ThreadBindEvent(size_t pPhysicalMemory, size_t pTotalStubCount);
    #ifdef DUMP_EVENT_TIMELINE
        pmStatus InitializeEventTimeline();
    #endif
		pmStatus ReduceSubtasks(pmTask* pTask, ulong pSubtaskId1, pmExecutionStub* pStub2, ulong pSubtaskId2);
		pmStatus StealSubtasks(pmTask* pTask, pmProcessingElement* pRequestingDevice, double pRequestingDeviceExecutionRate);
		pmStatus CancelAllSubtasks(pmTask* pTask, bool pTaskListeningOnCancellation);
        pmStatus CancelSubtaskRange(pmSubtaskRange& pRange);
        pmStatus ProcessNegotiatedRange(pmSubtaskRange& pRange);
        void ProcessDeferredShadowMemCommits(pmTask* pTask);
        void ReductionFinishEvent(pmTask* pTask);
        void ProcessRedistributionBucket(pmTask* pTask, size_t pBucketIndex);
        void FreeTaskResources(pmTask* pTask);

        pmStatus NegotiateRange(pmProcessingElement* pRequestingDevice, pmSubtaskRange& pRange);

        bool RequiresPrematureExit();

        void MarkInsideLibraryCode();
        void MarkInsideUserCode();
    
        void SetupJmpBuf(sigjmp_buf* pJmpBuf);
        void UnsetupJmpBuf(bool pHasJumped);
    
        void WaitForNetworkFetch(std::vector<pmCommunicatorCommandPtr>& pNetworkCommands);
        pmStatus FindSubtaskMemDependencies(pmTask* pTask, ulong pSubtaskId);
    
	protected:
		bool IsHighPriorityEventWaiting(ushort pPriority);
		pmStatus CommonPreExecuteOnCPU(pmTask* pTask, ulong pSubtaskId, bool pIsMultiAssign, bool pPrefetch);

		pmStatus FreeGpuResources();

		virtual pmStatus DoSubtaskReduction(pmTask* pTask, ulong pSubtaskId1, pmExecutionStub* pStub2, ulong pSubtaskId2);

        virtual ulong FindCollectivelyExecutableSubtaskRangeEnd(const pmSubtaskRange& pSubtaskRange, bool pMultiAssign) = 0;
        virtual void PrepareForSubtaskRangeExecution(pmTask* pTask, ulong pStartSubtaskId, ulong pEndSubtaskId) = 0;
        virtual void CleanupPostSubtaskRangeExecution(pmTask* pTask, bool pIsMultiAssign, ulong pStartSubtaskId, ulong pEndSubtaskId, bool pSuccess) = 0;
        virtual void TerminateUserModeExecution() = 0;

	private:
        void CheckTermination();
    
        typedef struct currentSubtaskRangeStats
        {
            pmTask* task;
            ulong startSubtaskId;
            ulong endSubtaskId;
            ulong parentRangeStartSubtask;
            bool originalAllottee;
            double startTime;
            bool reassigned;    // the current subtask range has been negotiated
            bool forceAckFlag;  // send acknowledgement for the entire parent range after current subtask range is stolen/negotiated
            bool prematureTermination;
            bool taskListeningOnCancellation;
            sigjmp_buf* jmpBuf;
            pmAccumulatorCommandPtr* accumulatorCommandPtr;
        
            currentSubtaskRangeStats(pmTask* pTask, ulong pStartSubtaskId, ulong pEndSubtaskId, bool pOriginalAllottee, ulong pParentRangeStartSubtask, sigjmp_buf* pJmpBuf, double pStartTime);
            
            void ResetEndSubtaskId(ulong pEndSubtaskId);
        } currentSubtaskRangeStats;
    
        typedef class currentSubtaskRangeTerminus
        {
            public:
                currentSubtaskRangeTerminus(bool& pReassigned, bool& pForceAckFlag, bool& pPrematureTermination, pmExecutionStub* pStub);
                void Terminating(currentSubtaskRangeStats* pStats);
        
            private:
                bool& mReassigned;
                bool& mForceAckFlag;
                bool& mPrematureTermination;
                pmExecutionStub* mStub;
        } currentSubtaskRangeTerminus;
    
		pmStatus ProcessEvent(execStub::stubEvent& pEvent);
        virtual pmStatus Execute(pmTask* pTask, ulong pSubtaskId, bool pIsMultiAssign, ulong* pPreftechSubtaskIdPtr) = 0;

    #ifdef DUMP_EVENT_TIMELINE
        ulong ExecuteWrapper(const pmSubtaskRange& pCurrentRange, execStub::stubEvent& pEvent, bool pIsMultiAssign, pmSubtaskRangeExecutionTimelineAutoPtr& pRangeExecTimelineAutoPtr, bool& pReassigned, bool& pForceAckFlag, bool& pPrematureTermination, pmStatus& pStatus);
    #else
        ulong ExecuteWrapper(const pmSubtaskRange& pCurrentRange, execStub::stubEvent& pEvent, bool pIsMultiAssign, bool& pReassigned, bool& pForceAckFlag, bool& pPrematureTermination, pmStatus& pStatus);
    #endif

        void PostHandleRangeExecutionCompletion(pmSubtaskRange& pRange, pmStatus pExecStatus);
        void HandleRangeExecutionCompletion(pmSubtaskRange& pRange, pmStatus pExecStatus);
        pmStatus CommonPostNegotiationOnCPU(pmTask* pTask, ulong pSubtaskId, bool pIsMultiAssign);
        void CommitRange(pmSubtaskRange& pRange, pmStatus pExecStatus);
        void CommitSubtaskShadowMem(pmTask* pTask, ulong pSubtaskId);
        void DeferShadowMemCommit(pmTask* pTask, ulong pSubtaskId);
        void CancelCurrentlyExecutingSubtaskRange(bool pTaskListeningOnCancellation);
        void TerminateCurrentSubtaskRange();

    #ifdef DUMP_EVENT_TIMELINE
        std::string GetEventTimelineName();
    #endif
    
		uint mDeviceIndexOnMachine;

        volatile sig_atomic_t mExecutingLibraryCode;
        RESOURCE_LOCK_IMPLEMENTATION_CLASS mCurrentSubtaskRangeLock;
        currentSubtaskRangeStats* mCurrentSubtaskRangeStats;  // Subtasks currently being executed
        std::map<std::pair<pmTask*, ulong>, std::vector<pmProcessingElement*> > mSecondaryAllotteeMap;  // PULL model: secondary allottees of a subtask range
    
        RESOURCE_LOCK_IMPLEMENTATION_CLASS mDeferredShadowMemCommitsLock;
        std::map<pmTask*, std::vector<ulong> > mDeferredShadowMemCommits;

    #ifdef DUMP_EVENT_TIMELINE
        std::auto_ptr<pmEventTimeline> mEventTimelineAutoPtr;
    #endif
};

class pmStubGPU : public pmExecutionStub
{
	public:
		pmStubGPU(uint pDeviceIndexOnMachine);
		virtual ~pmStubGPU();

		virtual pmStatus BindToProcessingElement() = 0;

		virtual std::string GetDeviceName() = 0;
		virtual std::string GetDeviceDescription() = 0;

		virtual pmDeviceType GetType() = 0;

		virtual pmStatus FreeResources() = 0;
		virtual pmStatus FreeExecutionResources() = 0;

		virtual pmStatus Execute(pmTask* pTask, ulong pSubtaskId, bool pIsMultiAssign, ulong* pPreftechSubtaskIdPtr) = 0;

    protected:
        virtual ulong FindCollectivelyExecutableSubtaskRangeEnd(const pmSubtaskRange& pSubtaskRange, bool pMultiAssign) = 0;
        virtual void PrepareForSubtaskRangeExecution(pmTask* pTask, ulong pStartSubtaskId, ulong pEndSubtaskId) = 0;
        virtual void CleanupPostSubtaskRangeExecution(pmTask* pTask, bool pIsMultiAssign, ulong pStartSubtaskId, ulong pEndSubtaskId, bool pSuccess) = 0;
        virtual void TerminateUserModeExecution() = 0;

	private:
};

class pmStubCPU : public pmExecutionStub
{
	public:
		pmStubCPU(size_t pCoreId, uint pDeviceIndexOnMachine);
		virtual ~pmStubCPU();

		virtual pmStatus BindToProcessingElement();
		virtual size_t GetCoreId();

		virtual std::string GetDeviceName();
		virtual std::string GetDeviceDescription();

		virtual pmDeviceType GetType();

		virtual pmStatus Execute(pmTask* pTask, ulong pSubtaskId, bool pIsMultiAssign, ulong* pPreftechSubtaskIdPtr);

    protected:
        virtual ulong FindCollectivelyExecutableSubtaskRangeEnd(const pmSubtaskRange& pSubtaskRange, bool pMultiAssign);
        virtual void PrepareForSubtaskRangeExecution(pmTask* pTask, ulong pStartSubtaskId, ulong pEndSubtaskId);
        virtual void CleanupPostSubtaskRangeExecution(pmTask* pTask, bool pIsMultiAssign, ulong pStartSubtaskId, ulong pEndSubtaskId, bool pSuccess);
        virtual void TerminateUserModeExecution();
    
	private:
 		size_t mCoreId;
};

#ifdef SUPPORT_CUDA
class pmStubCUDA : public pmStubGPU
{
    friend class pmDispatcherCUDA;

    public:
        pmStubCUDA(size_t pDeviceIndex, uint pDeviceIndexOnMachine);
		virtual ~pmStubCUDA();

		virtual pmStatus BindToProcessingElement();

		virtual std::string GetDeviceName();
		virtual std::string GetDeviceDescription();

		virtual pmDeviceType GetType();

		virtual pmStatus FreeResources();
		virtual pmStatus FreeExecutionResources();

		virtual pmStatus Execute(pmTask* pTask, ulong pSubtaskId, bool pIsMultiAssign, ulong* pPreftechSubtaskIdPtr);
    
        void* GetDeviceInfoCudaPtr();
        pmLastCudaExecutionRecord& GetLastExecutionRecord();
        void FreeTaskResources(pmMachine* pOriginatingHost, ulong pSequenceNumber);
    
        void StreamFinishCallback();

    #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
        void ReservePinnedMemory(size_t pPhysicalMemory, size_t pTotalStubCount);
        pmMemChunk* GetPinnedBufferChunk();
    #endif
    
        size_t GetDeviceIndex();

    protected:
        virtual ulong FindCollectivelyExecutableSubtaskRangeEnd(const pmSubtaskRange& pSubtaskRange, bool pMultiAssign);
        virtual void PrepareForSubtaskRangeExecution(pmTask* pTask, ulong pStartSubtaskId, ulong pEndSubtaskId);
        virtual void CleanupPostSubtaskRangeExecution(pmTask* pTask, bool pIsMultiAssign, ulong pStartSubtaskId, ulong pEndSubtaskId, bool pSuccess);
        virtual void TerminateUserModeExecution();

        void PopulateMemcpyCommands(pmTask* pTask, ulong pSubtaskId, pmSubtaskInfo& pSubtaskInfo, bool pOutputMemWriteOnly);
    
    #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
        void CopyDataToPinnedBuffers(pmTask* pTask, ulong pSubtaskId, pmSubtaskInfo& pSubtaskInfo, bool pOutputMemWriteOnly);
        pmStatus CopyDataFromPinnedBuffers(pmTask* pTask, ulong pSubtaskId, pmSubtaskInfo& pSubtaskInfo);
    #endif

	private:
		size_t mDeviceIndex;

        std::map<std::pair<pmMachine*, ulong>, pmTaskInfo> mTaskInfoCudaMap; // pair of task originating host and sequence number
        pmLastCudaExecutionRecord mLastExecutionRecord;
        void* mDeviceInfoCudaPtr;

        std::vector<std::vector<std::pair<size_t, size_t> > > mAllocationOffsets;   // pair of offset and size
        std::map<ulong, std::vector<void*> > mCudaPointersMap;
        size_t mTotalAllocationSize;
        void* mCudaAllocation;
        const size_t mMemElements;    
        size_t mPendingStreams;
        std::auto_ptr<SIGNAL_WAIT_IMPLEMENTATION_CLASS> mStreamSignalWait;
    
        std::vector<pmCudaMemcpyCommand> mDeviceToHostCommands;
        std::vector<pmCudaMemcpyCommand> mHostToDeviceCommands;
    
     #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
        std::auto_ptr<pmMemChunk> mPinnedBufferChunk;
        std::map<ulong, std::vector<void*> > mPinnedPointersMap;
        void* mPinnedBuffer;
        void* mPinnedAllocation;
    #else
        const pmStatus mStatusCopySrc;
        pmStatus mStatusCopyDest;
    #endif
};
#endif

bool execEventMatchFunc(execStub::stubEvent& pEvent, void* pCriterion);

} // end namespace pm

#endif
