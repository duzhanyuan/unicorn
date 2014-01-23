
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

#include "pmExecutionStub.h"
#include "pmSignalWait.h"
#include "pmTask.h"
#include "pmDispatcherGPU.h"
#include "pmHardware.h"
#include "pmDevicePool.h"
#include "pmCommand.h"
#include "pmCallbackUnit.h"
#include "pmScheduler.h"
#include "pmAddressSpace.h"
#include "pmTaskManager.h"
#include "pmTls.h"
#include "pmLogger.h"
#include "pmMemoryManager.h"
#include "pmReducer.h"
#include "pmRedistributor.h"

#include <memory>
#include <string>
#include <sstream>
#include <algorithm>
#include <limits>
#include <functional>

#include SYSTEM_CONFIGURATION_HEADER // for sched_setaffinity

#define INVOKE_SAFE_THROW_ON_FAILURE(objectType, object, function, ...) \
{ \
	pmStatus dStatus = pmSuccess; \
	const objectType* dObject = object; \
	if(dObject) \
		dStatus = dObject->function(__VA_ARGS__); \
	if(dStatus != pmSuccess) \
		PMTHROW(pmUserErrorException()); \
}

#define THROW_IF_PARTIAL_RANGE_OVERLAP(start1, end1, start2, end2) \
{ \
    bool dTotalOverlap = (start1 == start2 && end1 == end2); \
    bool dNoOverlap = (end2 < start1) || (start2 > end1); \
    \
    if(!dTotalOverlap && !dNoOverlap) \
        PMTHROW(pmFatalErrorException()); \
}

namespace pm
{

using namespace execStub;

/* class pmExecutionStub */
pmExecutionStub::pmExecutionStub(uint pDeviceIndexOnMachine)
    : mDeviceIndexOnMachine(pDeviceIndexOnMachine)
    , mExecutingLibraryCode(1)
    , mCurrentSubtaskRangeLock __LOCK_NAME__("pmExecutionStub::mCurrentSubtaskRangeLock")
    , mCurrentSubtaskRangeStats(NULL)
    , mSecondaryAllotteeLock __LOCK_NAME__("pmExecutionStub::mSecondaryAllotteeLock")
    , mDeferredShadowMemCommitsLock __LOCK_NAME__("pmExecutionStub::mDeferredShadowMemCommitsLock")
{
}

pmExecutionStub::~pmExecutionStub()
{
#ifdef DUMP_THREADS
	pmLogger::GetLogger()->Log(pmLogger::MINIMAL, pmLogger::INFORMATION, "Shutting down stub thread");
#endif
}

const pmProcessingElement* pmExecutionStub::GetProcessingElement() const
{
	return pmDevicePool::GetDevicePool()->GetDeviceAtMachineIndex(PM_LOCAL_MACHINE, mDeviceIndexOnMachine);
}
    
void pmExecutionStub::ThreadBindEvent(size_t pPhysicalMemory, size_t pTotalStubCount)
{
	SwitchThread(std::shared_ptr<stubEvent>(new threadBindEvent(THREAD_BIND, pPhysicalMemory, pTotalStubCount)), MAX_CONTROL_PRIORITY);
}

#ifdef DUMP_EVENT_TIMELINE
void pmExecutionStub::InitializeEventTimeline()
{
	SwitchThread(std::shared_ptr<stubEvent>(new initTimelineEvent(INIT_EVENT_TIMELINE)), MAX_CONTROL_PRIORITY);
}
#endif

void pmExecutionStub::Push(const pmSubtaskRange& pRange)
{
	DEBUG_EXCEPTION_ASSERT(pRange.endSubtask >= pRange.startSubtask);

#ifdef SUPPORT_SPLIT_SUBTASKS
    if(pRange.task->GetSubtaskSplitter().IsSplitting(GetType()))
    {
        bool lIsMultiAssignRange = (pRange.task->IsMultiAssignEnabled() && pRange.originalAllottee != NULL);
        if(lIsMultiAssignRange)
        {
        #ifdef _DEBUG
            std::cout << "WARNING << Multi-Assign range with stub executing split subtasks. Ignoring !!!" << std::endl;
        #endif

            return;    // Range is dropped by not executing and not adding to the event queue
        }

        // PUSH expects one consolidated acknowledgement for the entire assigned range
        if(pRange.task->GetSchedulingModel() == scheduler::PUSH && !pRange.originalAllottee)
        {
            FINALIZE_RESOURCE_PTR(dPushAckLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mPushAckLock, Lock(), Unlock());

            if(mPushAckHolder.find(pRange.task) != mPushAckHolder.end())
                PMTHROW(pmFatalErrorException());   // Only one range of a task allowed at a time
            
            std::map<ulong, std::vector<pmExecutionStub*> > lMap;
            mPushAckHolder.insert(std::make_pair(pRange.task, std::make_pair(std::make_pair(pRange.startSubtask, pRange.endSubtask), lMap)));
        }
    }
#endif
    
	SwitchThread(std::shared_ptr<stubEvent>(new subtaskExecEvent(SUBTASK_EXEC, pRange, false, 0)), pRange.task->GetPriority());
}

void pmExecutionStub::ReduceSubtasks(pmTask* pTask, ulong pSubtaskId1, pmSplitInfo* pSplitInfo1, pmExecutionStub* pStub2, ulong pSubtaskId2, pmSplitInfo* pSplitInfo2)
{
    pmSplitData lSplitData1(pSplitInfo1);
    pmSplitData lSplitData2(pSplitInfo2);

	SwitchThread(std::shared_ptr<stubEvent>(new subtaskReduceEvent(SUBTASK_REDUCE, pTask, pSubtaskId1, pStub2, pSubtaskId2, lSplitData1, lSplitData2)), pTask->GetPriority());
}

void pmExecutionStub::ProcessNegotiatedRange(const pmSubtaskRange& pRange)
{
	SwitchThread(std::shared_ptr<stubEvent>(new negotiatedRangeEvent(NEGOTIATED_RANGE, pRange)), pRange.task->GetPriority());
}

/* This is an asynchronous call. Current subtask is not cancelled immediately. */
void pmExecutionStub::CancelAllSubtasks(pmTask* pTask, bool pTaskListeningOnCancellation)
{
    ushort lPriority = pTask->GetPriority();

    // There is atmost one range per task at a time
    std::shared_ptr<stubEvent> lTaskEvent;
    DeleteAndGetFirstMatchingCommand(lPriority, execEventMatchFunc, pTask, lTaskEvent);

    // Auto lock/unlock scope
    {
        FINALIZE_RESOURCE_PTR(dCurrentSubtaskLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCurrentSubtaskRangeLock, Lock(), Unlock());

        if(mCurrentSubtaskRangeStats && mCurrentSubtaskRangeStats->task == pTask)
            CancelCurrentlyExecutingSubtaskRange(pTaskListeningOnCancellation);
    }
    
    if(pTask->IsMultiAssignEnabled() && !pmTaskManager::GetTaskManager()->DoesTaskHavePendingSubtasks(pTask))
    {
        FINALIZE_RESOURCE_PTR(dSecondaryAllotteeLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mSecondaryAllotteeLock, Lock(), Unlock());

        auto lIter = mSecondaryAllotteeMap.begin(), lEndIter = mSecondaryAllotteeMap.end();
    
        for(; lIter != lEndIter; )
        {
            if(lIter->first.first == pTask)
                mSecondaryAllotteeMap.erase(lIter++);
            else
                ++lIter;
        }
    }
}
    
/* This is an asynchronous call. Current subtask is not cancelled immediately. */
void pmExecutionStub::CancelSubtaskRange(const pmSubtaskRange& pRange)
{
    ushort lPriority = pRange.task->GetPriority();
    
    std::shared_ptr<stubEvent> lTaskEvent;
    bool lFound = (DeleteAndGetFirstMatchingCommand(lPriority, execEventMatchFunc, pRange.task, lTaskEvent) == pmSuccess);

    // Auto lock/unlock scope
    {
        FINALIZE_RESOURCE_PTR(dCurrentSubtaskLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCurrentSubtaskRangeLock, Lock(), Unlock());
        
        if(mCurrentSubtaskRangeStats && mCurrentSubtaskRangeStats->task == pRange.task)
        {
            if(mCurrentSubtaskRangeStats->startSubtaskId >= pRange.startSubtask && mCurrentSubtaskRangeStats->endSubtaskId <= pRange.endSubtask)
                CancelCurrentlyExecutingSubtaskRange(false);
        }
    }
    
    if(lFound)
    {
        subtaskExecEvent& lExecEvent = static_cast<subtaskExecEvent&>(*lTaskEvent.get());
        if(pRange.endSubtask < lExecEvent.range.startSubtask || pRange.startSubtask > lExecEvent.range.endSubtask)
            SwitchThread(lTaskEvent, lPriority);
    }
}

#ifdef SUPPORT_CUDA
void pmExecutionStub::FreeGpuResources()
{
    SwitchThread(std::shared_ptr<stubEvent>(new freeGpuResourcesEvent(FREE_GPU_RESOURCES)), RESERVED_PRIORITY);
}
#endif
    
void pmExecutionStub::FreeTaskResources(pmTask* pTask)
{
#ifdef SUPPORT_SPLIT_SUBTASKS
    if(pTask->GetSchedulingModel() == scheduler::PUSH && pTask->GetSubtaskSplitter().IsSplitting(GetType()))
    {
        FINALIZE_RESOURCE_PTR(dPushAckLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mPushAckLock, Lock(), Unlock());

        mPushAckHolder.erase(pTask);
    }
#endif

#ifdef SUPPORT_CUDA
    if(dynamic_cast<pmStubCUDA*>(this))
        SwitchThread(std::shared_ptr<stubEvent>(new freeTaskResourcesEvent(FREE_TASK_RESOURCES, pTask->GetOriginatingHost(), pTask->GetSequenceNumber())), RESERVED_PRIORITY);
#endif
}
    
#ifdef SUPPORT_SPLIT_SUBTASKS
void pmExecutionStub::SplitSubtaskCheckEvent(pmTask* pTask)
{
    SwitchThread(std::shared_ptr<stubEvent>(new splitSubtaskCheckEvent(SPLIT_SUBTASK_CHECK, pTask)), pTask->GetPriority());
}
    
void pmExecutionStub::RemoveSplitSubtaskCheckEvent(pmTask* pTask)
{
    DeleteMatchingCommands(pTask->GetPriority(), splitSubtaskCheckEventMatchFunc, pTask);
    WaitIfCurrentCommandMatches(splitSubtaskCheckEventMatchFunc, pTask);
}
#endif
    
void pmExecutionStub::PostHandleRangeExecutionCompletion(pmSubtaskRange& pRange, pmStatus pExecStatus)
{
    SwitchThread(std::shared_ptr<stubEvent>(new execCompletionEvent(POST_HANDLE_EXEC_COMPLETION, pRange, pExecStatus)), pRange.task->GetPriority() - 1);
}
    
void pmExecutionStub::RemoteSubtaskReduce(pmTask* pTask, const pmCommunicatorCommandPtr& pCommandPtr)
{
    SwitchThread(std::shared_ptr<stubEvent>(new remoteSubtaskReduceEvent(REMOTE_SUBTASK_REDUCE, pTask, pCommandPtr)), pTask->GetPriority());
}

void pmExecutionStub::ProcessDeferredShadowMemCommits(pmTask* pTask)
{
    bool lPendingCommits = false;

    // Auto lock/unlock scope
    {
        FINALIZE_RESOURCE_PTR(dDeferredShadowMemCommitsLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mDeferredShadowMemCommitsLock, Lock(), Unlock());
        lPendingCommits = (mDeferredShadowMemCommits.find(pTask) != mDeferredShadowMemCommits.end());
    }

    if(lPendingCommits)
    {
        pTask->RecordStubWillSendShadowMemCommitMessage();
        SwitchThread(std::shared_ptr<stubEvent>(new deferredShadowMemCommitsEvent(DEFERRED_SHADOW_MEM_COMMITS, pTask)), pTask->GetPriority() - 1);
    }
}
    
void pmExecutionStub::ReductionFinishEvent(pmTask* pTask)
{
    SwitchThread(std::shared_ptr<stubEvent>(new reductionFinishEvent(REDUCTION_FINISH, pTask)), pTask->GetPriority());
}
    
void pmExecutionStub::ProcessRedistributionBucket(pmTask* pTask, uint pAddressSpaceIndex, size_t pBucketIndex)
{
    SwitchThread(std::shared_ptr<stubEvent>(new processRedistributionBucketEvent(PROCESS_REDISTRIBUTION_BUCKET, pTask, pAddressSpaceIndex, pBucketIndex)), pTask->GetPriority());
}

void pmExecutionStub::NegotiateRange(const pmProcessingElement* pRequestingDevice, const pmSubtaskRange& pRange)
{
    DEBUG_EXCEPTION_ASSERT(GetProcessingElement() == pRange.originalAllottee);

    ushort lPriority = pRange.task->GetPriority();
    
    std::shared_ptr<stubEvent> lTaskEvent;
    if(pRange.task->IsMultiAssignEnabled())
    {
    #ifdef SUPPORT_SPLIT_SUBTASKS
        if(pRange.task->GetSubtaskSplitter().IsSplitting(GetType()))
        {
            // When a split happens, the source stub always gets to execute the 0'th split. Other stubs get remaining splits when they demand.
            // Under PULL scheme, a multi-assign only happens from the currently executing subtask range which is always one subtask wide for
            // splitted subtasks. For PUSH model, the entire subtask range is multi-assigned by the owner host.
            if(pRange.task->GetSchedulingModel() == scheduler::PULL)
            {
                DEBUG_EXCEPTION_ASSERT(pRange.startSubtask == pRange.endSubtask);
                
                if(pRange.task->GetSubtaskSplitter().Negotiate(this, pRange.startSubtask))
                {
                #ifdef TRACK_MULTI_ASSIGN
                    std::cout << "[Host " << pmGetHostId() << "]: Split subtask negotiation success from device " << GetProcessingElement()->GetGlobalDeviceIndex() << " to device " << pRequestingDevice->GetGlobalDeviceIndex() << "; Negotiated range [" << pRange.startSubtask << ", " << pRange.endSubtask << "]" << std::endl;
                #endif

                    pmScheduler::GetScheduler()->SendRangeNegotiationSuccess(pRequestingDevice, pRange);
                }
            }
            else
            {
                FINALIZE_RESOURCE_PTR(dPushAckLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mPushAckLock, Lock(), Unlock());

                std::map<pmTask*, std::pair<std::pair<ulong, ulong>, std::map<ulong, std::vector<pmExecutionStub*> > > >::iterator lIter = mPushAckHolder.find(pRange.task);
                if(lIter != mPushAckHolder.end())
                {
                    if(lIter->second.first.first == pRange.startSubtask && lIter->second.first.second == pRange.endSubtask)
                    {
                        bool lNegotiationStatus = false;
                        bool lFound = (DeleteAndGetFirstMatchingCommand(lPriority, execEventMatchFunc, pRange.task, lTaskEvent) == pmSuccess);
                        if(lFound)
                        {
                            subtaskExecEvent& lExecEvent = static_cast<subtaskExecEvent&>(*lTaskEvent.get());
                            if(pRange.endSubtask < lExecEvent.range.startSubtask || pRange.startSubtask > lExecEvent.range.endSubtask)
                            {
                                SwitchThread(std::move(lTaskEvent), lPriority);
                            }
                            else
                            {
                                lNegotiationStatus = true;

                                for(ulong i = pRange.startSubtask; i < lExecEvent.range.startSubtask; ++i)
                                    pRange.task->GetSubtaskSplitter().Negotiate(this, i);
                            }
                        }
                        else
                        {
                            for(ulong i = pRange.startSubtask; i <= pRange.endSubtask; ++i)
                                lNegotiationStatus |= pRange.task->GetSubtaskSplitter().Negotiate(this, i);
                        }
                        
                        if(lNegotiationStatus)
                        {
                            #ifdef TRACK_MULTI_ASSIGN
                                std::cout << "[Host " << pmGetHostId() << "]: Split subtask negotiation success from device " << GetProcessingElement()->GetGlobalDeviceIndex() << " to device " << pRequestingDevice->GetGlobalDeviceIndex() << "; Negotiated range [" << pRange.startSubtask << ", " << pRange.endSubtask << "]" << std::endl;
                            #endif

                                pmScheduler::GetScheduler()->SendRangeNegotiationSuccess(pRequestingDevice, pRange);
                        }

                        mPushAckHolder.erase(lIter);
                    }
                }
            }

            return;
        }
    #endif
        
        if(pRange.task->GetSchedulingModel() == scheduler::PULL)
        {
            EXCEPTION_ASSERT(pRange.startSubtask == pRange.endSubtask);
        
            FINALIZE_RESOURCE_PTR(dCurrentSubtaskLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCurrentSubtaskRangeLock, Lock(), Unlock());
            
            if(mCurrentSubtaskRangeStats && mCurrentSubtaskRangeStats->task == pRange.task)
            {
                if(mCurrentSubtaskRangeStats->endSubtaskId == pRange.endSubtask && !mCurrentSubtaskRangeStats->reassigned)
                {
                    DEBUG_EXCEPTION_ASSERT(mCurrentSubtaskRangeStats->originalAllottee);

                    std::vector<const pmProcessingElement*> lSecondaryAllottees;

                    // Auto lock/unlock scope
                    {
                        FINALIZE_RESOURCE_PTR(dSecondaryAllotteeLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mSecondaryAllotteeLock, Lock(), Unlock());

                        auto lAllotteeIter = mSecondaryAllotteeMap.find(std::make_pair(pRange.task, pRange.endSubtask));
                        if(lAllotteeIter != mSecondaryAllotteeMap.end())
                        {
                            lSecondaryAllottees = std::move(lAllotteeIter->second);
                            mSecondaryAllotteeMap.erase(lAllotteeIter);
                        }
                    }

                    DEBUG_EXCEPTION_ASSERT(std::find(lSecondaryAllottees.begin(), lSecondaryAllottees.end(), pRequestingDevice) != lSecondaryAllottees.end())
                
                #ifdef TRACK_MULTI_ASSIGN
                    std::cout << "[Host " << pmGetHostId() << "]: Range negotiation success from device " << GetProcessingElement()->GetGlobalDeviceIndex() << " to device " << pRequestingDevice->GetGlobalDeviceIndex() << "; Negotiated range [" << pRange.startSubtask << ", " << pRange.endSubtask << "]" << std::endl;
                #endif
                
                    pmScheduler::GetScheduler()->SendRangeNegotiationSuccess(pRequestingDevice, pRange);
                    mCurrentSubtaskRangeStats->reassigned = true;
                    CancelCurrentlyExecutingSubtaskRange(false);
                            
                    if(mCurrentSubtaskRangeStats->parentRangeStartSubtask != mCurrentSubtaskRangeStats->endSubtaskId)
                    {
                        pmSubtaskRange lCompletedRange(pRange.task, NULL, mCurrentSubtaskRangeStats->parentRangeStartSubtask, mCurrentSubtaskRangeStats->endSubtaskId - 1);

                        PostHandleRangeExecutionCompletion(lCompletedRange, pmSuccess);
                    }
                
                    pmScheduler::GetScheduler()->SendSubtaskRangeCancellationMessage(pRange.originalAllottee, pRange);
                    
                    for_each(lSecondaryAllottees, [&] (const pmProcessingElement* pElement)
                    {
                        if(pElement != pRequestingDevice)
                            pmScheduler::GetScheduler()->SendSubtaskRangeCancellationMessage(pElement, pRange);
                    });
                }
            }
        }
        else
        {
            pmSubtaskRange lNegotiatedRange(pRange);
            bool lSuccessfulNegotiation = false;
            bool lCurrentTransferred = false;
        
            bool lFound = (DeleteAndGetFirstMatchingCommand(lPriority, execEventMatchFunc, pRange.task, lTaskEvent) == pmSuccess);

            FINALIZE_RESOURCE_PTR(dCurrentSubtaskLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCurrentSubtaskRangeLock, Lock(), Unlock());
        
            bool lConsiderCurrent = (mCurrentSubtaskRangeStats && mCurrentSubtaskRangeStats->task == pRange.task && mCurrentSubtaskRangeStats->startSubtaskId >= pRange.startSubtask && mCurrentSubtaskRangeStats->endSubtaskId <= pRange.endSubtask && !mCurrentSubtaskRangeStats->reassigned);
                
            if(lFound)
            {
                subtaskExecEvent& lExecEvent = static_cast<subtaskExecEvent&>(*lTaskEvent.get());
                if(lExecEvent.range.originalAllottee == NULL)
                {       
                    if(pRange.endSubtask < lExecEvent.range.startSubtask || pRange.startSubtask > lExecEvent.range.endSubtask)
                    {
                        SwitchThread(std::move(lTaskEvent), lPriority);
                    }
                    else
                    {
                        ulong lFirstPendingSubtask = (lExecEvent.rangeExecutedOnce ? (lExecEvent.lastExecutedSubtaskId + 1) : lExecEvent.range.startSubtask);
                        ulong lLastPendingSubtask = lExecEvent.range.endSubtask;
                    
                        if(lConsiderCurrent)
                        {
                            DEBUG_EXCEPTION_ASSERT(mCurrentSubtaskRangeStats->originalAllottee);
                            DEBUG_EXCEPTION_ASSERT(lExecEvent.lastExecutedSubtaskId == mCurrentSubtaskRangeStats->endSubtaskId);
                        
                            lFirstPendingSubtask -=  (mCurrentSubtaskRangeStats->endSubtaskId - mCurrentSubtaskRangeStats->startSubtaskId + 1);
                            lCurrentTransferred = true;
                        }
                    
                        lNegotiatedRange.startSubtask = std::max(pRange.startSubtask, lFirstPendingSubtask);
                        lNegotiatedRange.endSubtask = std::min(pRange.endSubtask, lLastPendingSubtask);
                    
                        lSuccessfulNegotiation = true;

                        DEBUG_EXCEPTION_ASSERT(lNegotiatedRange.startSubtask <= lNegotiatedRange.endSubtask && lNegotiatedRange.endSubtask >= lLastPendingSubtask);
                    
                        if(lNegotiatedRange.startSubtask != lExecEvent.range.startSubtask)  // Entire range not negotiated
                        {
                            // Find range left with original allottee
                            lExecEvent.range.endSubtask = lNegotiatedRange.startSubtask - 1;
                            if(lConsiderCurrent && lExecEvent.range.endSubtask >= mCurrentSubtaskRangeStats->endSubtaskId)
                                lCurrentTransferred = false;   // current subtask still with original allottee
                        
                            bool lCurrentSubtaskInRemainingRange = (lConsiderCurrent && !lCurrentTransferred);

                            if(!lCurrentSubtaskInRemainingRange && lExecEvent.range.endSubtask == (lExecEvent.lastExecutedSubtaskId - (lCurrentTransferred ? (mCurrentSubtaskRangeStats->endSubtaskId - mCurrentSubtaskRangeStats->startSubtaskId + 1) : 0)))  // no pending subtask
                            {
                                if(lExecEvent.rangeExecutedOnce)
                                    PostHandleRangeExecutionCompletion(lExecEvent.range, pmSuccess);
                            }
                            else if(lCurrentSubtaskInRemainingRange && lExecEvent.lastExecutedSubtaskId == lExecEvent.range.endSubtask) // only current subtask range pending
                            {
                                DEBUG_EXCEPTION_ASSERT(lExecEvent.lastExecutedSubtaskId == mCurrentSubtaskRangeStats->endSubtaskId);
                            
                                mCurrentSubtaskRangeStats->forceAckFlag = true;  // send acknowledgement of the done range after current subtask range finishes
                            }
                            else
                            {   // pending range does not have current subtask or has more subtasks after the current one
                                SwitchThread(std::move(lTaskEvent), lPriority);
                            }
                        }
                    }
                }
                else
                {
                    SwitchThread(std::move(lTaskEvent), lPriority);
                }
            }
            else if(lConsiderCurrent)
            {
            #ifdef _DEBUG
                if(!mCurrentSubtaskRangeStats->originalAllottee)
                    PMTHROW(pmFatalErrorException());
            #endif
            
                lSuccessfulNegotiation = true;
                lCurrentTransferred = true;
            
                lNegotiatedRange.startSubtask = mCurrentSubtaskRangeStats->startSubtaskId;
                lNegotiatedRange.endSubtask = mCurrentSubtaskRangeStats->endSubtaskId;
            
                if(mCurrentSubtaskRangeStats->parentRangeStartSubtask != mCurrentSubtaskRangeStats->startSubtaskId)
                {
                    pmSubtaskRange lCompletedRange(pRange.task, NULL, mCurrentSubtaskRangeStats->parentRangeStartSubtask, mCurrentSubtaskRangeStats->startSubtaskId - 1);
                    PostHandleRangeExecutionCompletion(lCompletedRange, pmSuccess);
                }
            }

            if(lSuccessfulNegotiation)
            {
            #ifdef TRACK_MULTI_ASSIGN
                std::cout << "[Host " << pmGetHostId() << "]: Range negotiation success from device " << GetProcessingElement()->GetGlobalDeviceIndex() << " to device " << pRequestingDevice->GetGlobalDeviceIndex() << "; Negotiated range [" << lNegotiatedRange.startSubtask << ", " << lNegotiatedRange.endSubtask << "]" << std::endl;
            #endif
            
                pmScheduler::GetScheduler()->SendRangeNegotiationSuccess(pRequestingDevice, lNegotiatedRange);
            }

            if(lCurrentTransferred)
            {
                mCurrentSubtaskRangeStats->reassigned = true;
                CancelCurrentlyExecutingSubtaskRange(false);
            }
        }
    }
#ifdef _DEBUG
    else
    {
        PMTHROW(pmFatalErrorException());
    }
#endif
}

// This method must be called with mCurrentSubtaskRangeLock acquired
ulong pmExecutionStub::GetStealCount(pmTask* pTask, const pmProcessingElement* pRequestingDevice, ulong pAvailableSubtasks, double pLocalExecutionRate, double pRequestingDeviceExecutionRate, bool pStrict)
{
    ulong lStealCount = 0;
    const float lStrictnessFactor = ((1.0 / pRequestingDeviceExecutionRate) < 0.5) ? 5.0 : 3.0;
    const float lOverheadFactor = 1.05; // 5 percent

    if(pAvailableSubtasks)
    {
        double lTotalExecRate = pLocalExecutionRate + pRequestingDeviceExecutionRate;
    
        if(pLocalExecutionRate == (double)0.0)
        {
            if(mCurrentSubtaskRangeStats && mCurrentSubtaskRangeStats->task == pTask)
            {
                double lElapsedTime = pmBase::GetCurrentTimeInSecs() - mCurrentSubtaskRangeStats->startTime;
                
                if(pStrict)
                {
                    if(lElapsedTime < lStrictnessFactor * (1.0 / pRequestingDeviceExecutionRate) * STEAL_WAIT_FACTOR)
                        lStealCount = 0;
                    else
                        lStealCount = 1;
                }
                else
                {
                    if(lElapsedTime < (1.0 / pRequestingDeviceExecutionRate) * STEAL_WAIT_FACTOR)
                        lStealCount = 0;
                    else if(lElapsedTime < (1.0 / pRequestingDeviceExecutionRate) * STEAL_WAIT_FACTOR * STEAL_WAIT_FACTOR)
                        lStealCount = 1;
                    else
                        lStealCount = pAvailableSubtasks;
                }
            }
            else
            {
                if(!pStrict)
                    lStealCount = pAvailableSubtasks;
            }
        }
        else
        {
            double lTotalExecutionTimeRequired = pAvailableSubtasks / lTotalExecRate;	// if subtasks are divided between both devices, how much time reqd
            double lLocalExecutionTimeForAllSubtasks = pAvailableSubtasks / pLocalExecutionRate;	// if all subtasks are executed locally, how much time it will take
            double lDividedExecutionTimeForAllSubtasks = lTotalExecutionTimeRequired * lOverheadFactor;
            
            if(lLocalExecutionTimeForAllSubtasks > lDividedExecutionTimeForAllSubtasks)
            {
                double lTimeDiff = lLocalExecutionTimeForAllSubtasks - lDividedExecutionTimeForAllSubtasks;
                lStealCount = (ulong)(lTimeDiff * pLocalExecutionRate);
            }
        }
    }
    
#ifdef SUPPORT_SPLIT_SUBTASKS
    if(lStealCount)
    {
        // Currently subtask splitter sends acknowledgement every executing every subtask. This causes
        // a problem for PULL scheduling because an acknowledgement leads to generation of a steal request.
        // Now if a stolen range with more than one subtasks is assigned to a split executor, then the
        // subsequent steal request after executing the first subtask of this range may lead to stealing
        // another new range. This will add two ranges of same task in the stub queue. This is not supported.
        if(pTask->GetSubtaskSplitter().IsSplitting(pRequestingDevice->GetType()))
            lStealCount = 1;
    }
#endif
    
    return lStealCount;
}
    
void pmExecutionStub::StealSubtasks(pmTask* pTask, const pmProcessingElement* pRequestingDevice, double pRequestingDeviceExecutionRate)
{
    bool lStealSuccess = false;
    ushort lPriority = pTask->GetPriority();
    const pmProcessingElement* lLocalDevice = GetProcessingElement();
    double lLocalRate = pTask->GetTaskExecStats().GetStubExecutionRate(this);
    
    std::shared_ptr<stubEvent> lTaskEvent;
    bool lFound = (DeleteAndGetFirstMatchingCommand(lPriority, execEventMatchFunc, pTask, lTaskEvent) == pmSuccess);
    
    FINALIZE_RESOURCE_PTR(dCurrentSubtaskLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCurrentSubtaskRangeLock, Lock(), Unlock());
    
    if(lFound)
    {
        subtaskExecEvent& lExecEvent = static_cast<subtaskExecEvent&>(*lTaskEvent.get());
        if(!pTask->IsMultiAssignEnabled() || lExecEvent.range.originalAllottee == NULL)
        {
            ulong lAvailableSubtasks = ((lExecEvent.rangeExecutedOnce) ? (lExecEvent.range.endSubtask - lExecEvent.lastExecutedSubtaskId) : (lExecEvent.range.endSubtask - lExecEvent.range.startSubtask + 1));

            ulong lStealCount = GetStealCount(pTask, pRequestingDevice, lAvailableSubtasks, lLocalRate, pRequestingDeviceExecutionRate, false);
            
            if(lStealCount)
            {                        
                pmSubtaskRange lStolenRange(pTask, NULL, (lExecEvent.range.endSubtask - lStealCount) + 1, lExecEvent.range.endSubtask);
                
                lExecEvent.range.endSubtask -= lStealCount;
                
                bool lCurrentSubtaskInRemainingRange = false;
                if(mCurrentSubtaskRangeStats && mCurrentSubtaskRangeStats->task == pTask && lExecEvent.range.startSubtask <= mCurrentSubtaskRangeStats->startSubtaskId && mCurrentSubtaskRangeStats->endSubtaskId <= lExecEvent.range.endSubtask)
                {
                    DEBUG_EXCEPTION_ASSERT(mCurrentSubtaskRangeStats->originalAllottee && !mCurrentSubtaskRangeStats->reassigned);
                
                    lCurrentSubtaskInRemainingRange = true;
                }
                
                if(!lCurrentSubtaskInRemainingRange && lExecEvent.lastExecutedSubtaskId == lExecEvent.range.endSubtask) // no pending subtask
                {
                    if(lExecEvent.rangeExecutedOnce)
                        PostHandleRangeExecutionCompletion(lExecEvent.range, pmSuccess);
                }
                else if(lCurrentSubtaskInRemainingRange && lExecEvent.lastExecutedSubtaskId == lExecEvent.range.endSubtask) // only current subtask range pending
                {
                    DEBUG_EXCEPTION_ASSERT(lExecEvent.lastExecutedSubtaskId == mCurrentSubtaskRangeStats->endSubtaskId);

                    mCurrentSubtaskRangeStats->forceAckFlag = true;  // send acknowledgement of the done range after current subtask range finishes
                }
                else // pending range does not have current subtask or has more subtasks after the current one
                {
                    if(lExecEvent.rangeExecutedOnce || !(lStolenRange.startSubtask == lExecEvent.range.startSubtask && lStolenRange.endSubtask == lExecEvent.range.endSubtask + lStealCount))
                        SwitchThread(std::move(lTaskEvent), lPriority);
                }
                
                lStealSuccess = true;
                pmScheduler::GetScheduler()->StealSuccessEvent(pRequestingDevice, lLocalDevice, lStolenRange);
            }
            else
            {
                SwitchThread(std::move(lTaskEvent), lPriority);
            }
        }
        else
        {
            SwitchThread(std::move(lTaskEvent), lPriority);
        }
    }
    else if(pTask->IsMultiAssignEnabled())
    {
        if(mCurrentSubtaskRangeStats && mCurrentSubtaskRangeStats->task == pTask && mCurrentSubtaskRangeStats->originalAllottee && !mCurrentSubtaskRangeStats->reassigned)
        {
            if(!(pRequestingDevice->GetMachine() == PM_LOCAL_MACHINE && pRequestingDevice->GetType() == GetType()))
            {
                pmExecutionStub* lSecondaryAllotteeMapStub = this;
                
            #ifdef SUPPORT_SPLIT_SUBTASKS
                if(mCurrentSubtaskRangeStats->splitData.valid)
                {
                    DEBUG_EXCEPTION_ASSERT(mCurrentSubtaskRangeStats->splitSubtaskSourceStub);
                    DEBUG_EXCEPTION_ASSERT(mCurrentSubtaskRangeStats->startSubtaskId == mCurrentSubtaskRangeStats->endSubtaskId);
                    
                    lSecondaryAllotteeMapStub = mCurrentSubtaskRangeStats->splitSubtaskSourceStub;
                }
            #endif

                std::pair<pmTask*, ulong> lPair(pTask, mCurrentSubtaskRangeStats->endSubtaskId);

                FINALIZE_RESOURCE_PTR(dSecondaryAllotteeLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &lSecondaryAllotteeMapStub->mSecondaryAllotteeLock, Lock(), Unlock());

                auto& lSecondaryAllotteeMap = lSecondaryAllotteeMapStub->mSecondaryAllotteeMap;
                
                auto lAllotteeIter = lSecondaryAllotteeMap.find(lPair);
                if((lAllotteeIter == lSecondaryAllotteeMap.end()) || (lAllotteeIter->second.size() < MAX_SUBTASK_MULTI_ASSIGN_COUNT - 1))
                {
                    ulong lSubtaskCount = mCurrentSubtaskRangeStats->endSubtaskId - mCurrentSubtaskRangeStats->startSubtaskId + 1;
                    ulong lPendingExecutions = (mCurrentSubtaskRangeStats->currentSubtaskIdValid ? (mCurrentSubtaskRangeStats->endSubtaskId - mCurrentSubtaskRangeStats->currentSubtaskId) : (lSubtaskCount - 1));  // Using lSubtaskCount - 1 as lSubtaskCount may result in stealing all subtasks which will leave 0 subtasks with current device. This is not implemented gracefully yet.

                    if(lPendingExecutions)
                    {
                        ulong lStealCount = GetStealCount(pTask, pRequestingDevice, lPendingExecutions, lLocalRate, pRequestingDeviceExecutionRate, true);
                        
                        if(lStealCount)
                        {
                            DEBUG_EXCEPTION_ASSERT(mCurrentSubtaskRangeStats->currentSubtaskIdValid || lStealCount != lSubtaskCount);  // All can't be stolen

                            pmSubtaskRange lStolenRange(pTask, NULL, (mCurrentSubtaskRangeStats->endSubtaskId - lStealCount) + 1, mCurrentSubtaskRangeStats->endSubtaskId);
                        
                            mCurrentSubtaskRangeStats->endSubtaskId -= lStealCount;
                            mCurrentSubtaskRangeStats->forceAckFlag = true;  // send acknowledgement of the done range after current subtask range finishes
                            
                            DEBUG_EXCEPTION_ASSERT(mCurrentSubtaskRangeStats->endSubtaskId >= mCurrentSubtaskRangeStats->startSubtaskId);

                            lStealSuccess = true;
                            pmScheduler::GetScheduler()->StealSuccessEvent(pRequestingDevice, lLocalDevice, lStolenRange);
                        }
                    }
                    else
                    {
                        bool lShouldMultiAssign = true;

                    #ifdef SUPPORT_CUDA
                        // If the target device is a GPU and has actually started executing the subtask, then
                        // there is no need to multi assign because even if the source device finishes early
                        // after multi-assign, the target one can't actually cancel the subtask execution.
                        
                        if(GetType() == GPU_CUDA && mCurrentSubtaskRangeStats->currentSubtaskInPostDataFetchStage)
                            lShouldMultiAssign = false;
                    #endif

                    #ifdef SUPPORT_SPLIT_SUBTASKS
                        // Currently subtask splitter does not execute multi-assign ranges
                        if(pTask->GetSubtaskSplitter().IsSplitting(pRequestingDevice->GetType()))
                            lShouldMultiAssign = false;
                    #endif
                        
                        if(lShouldMultiAssign)
                        {
                            ulong lMultiAssignSubtaskCount = 1;

                            bool lLocalRateZero = (lLocalRate == (double)0.0);
                            double lElapsedTime = pmBase::GetCurrentTimeInSecs() - mCurrentSubtaskRangeStats->startTime;
                            double lTransferOverheadTime = 0;   // add network and other overheads here
                            double lExpectedRemoteTimeToExecute = (lMultiAssignSubtaskCount/pRequestingDeviceExecutionRate);
                            double lExpectedLocalTimeToFinish = (lLocalRateZero ? 0.0 : ((lMultiAssignSubtaskCount/lLocalRate) - lElapsedTime));
                        
                            lShouldMultiAssign = (lLocalRateZero ? (lElapsedTime >= (1.0 / pRequestingDeviceExecutionRate) * MA_WAIT_FACTOR) : (lExpectedRemoteTimeToExecute + lTransferOverheadTime < lExpectedLocalTimeToFinish));
                                
                            if(lShouldMultiAssign)
                            {
                            #ifdef SUPPORT_SPLIT_SUBTASKS
                                if(mCurrentSubtaskRangeStats->splitData.valid)
                                {
                                    lLocalDevice = lSecondaryAllotteeMapStub->GetProcessingElement();

                                    if(!lSecondaryAllotteeMapStub->UpdateSecondaryAllotteeMapInternal(lPair, pRequestingDevice))
                                        return;
                                }
                                else
                            #endif
                                {
                                    lSecondaryAllotteeMap[lPair].push_back(pRequestingDevice);
                                }

                                pmSubtaskRange lMultiAssignRange(pTask, lLocalDevice, mCurrentSubtaskRangeStats->endSubtaskId, mCurrentSubtaskRangeStats->endSubtaskId);

                            #ifdef TRACK_MULTI_ASSIGN
                                std::cout << "Multiassign of subtask range [" << mCurrentSubtaskRangeStats->endSubtaskId << " - " << mCurrentSubtaskRangeStats->endSubtaskId << "] from range [" << mCurrentSubtaskRangeStats->parentRangeStartSubtask << " - " << mCurrentSubtaskRangeStats->endSubtaskId << "+] - Device " << pRequestingDevice->GetGlobalDeviceIndex() << ", Original Allottee - Device " << lLocalDevice->GetGlobalDeviceIndex() << ", Secondary allottment count - " << lSecondaryAllotteeMap[lPair].size() << std::endl;
                            #endif

                                lStealSuccess = true;
                                pmScheduler::GetScheduler()->StealSuccessEvent(pRequestingDevice, lLocalDevice, lMultiAssignRange);
                            }
                        }
                    }
                }
            }
        }
    }

    if(!lStealSuccess)
        pmScheduler::GetScheduler()->StealFailedEvent(pRequestingDevice, lLocalDevice, pTask);
}

void pmExecutionStub::ThreadSwitchCallback(std::shared_ptr<stubEvent>& pEvent)
{
	try
	{
		ProcessEvent(*pEvent);
	}
    catch(pmException& e)
    {
        pmLogger::GetLogger()->Log(pmLogger::MINIMAL, pmLogger::WARNING, "Exception generated from stub thread");
    }
}

void pmExecutionStub::ProcessEvent(stubEvent& pEvent)
{
	switch(pEvent.eventId)
	{
		case THREAD_BIND:
		{
			BindToProcessingElement();
            TLS_IMPLEMENTATION_CLASS::GetTls()->SetThreadLocalStorage(TLS_EXEC_STUB, this);

        #ifdef SUPPORT_CUDA
            pmStubCUDA* lStub = dynamic_cast<pmStubCUDA*>(this);
            if(lStub)
            {
                threadBindEvent& lEvent = static_cast<threadBindEvent&>(pEvent);
                lStub->ReserveMemory(lEvent.physicalMemory, lEvent.totalStubCount);
            }
        #endif

			break;
		}
        
    #ifdef DUMP_EVENT_TIMELINE
        case INIT_EVENT_TIMELINE:
        {
            mEventTimelineAutoPtr.reset(new pmEventTimeline(GetEventTimelineName()));
        
            break;
        }
    #endif
        
		case SUBTASK_EXEC:	/* Comes from scheduler thread */
		{
            subtaskExecEvent& lEvent = static_cast<subtaskExecEvent&>(pEvent);

        #ifdef SUPPORT_SPLIT_SUBTASKS
            if(CheckSplittedExecution(lEvent))
                break;
        #endif
            
            ExecuteSubtaskRange(lEvent);

			break;
		}

		case SUBTASK_REDUCE:
		{
            subtaskReduceEvent& lEvent = static_cast<subtaskReduceEvent&>(pEvent);

            std::unique_ptr<pmSplitInfo> lSplitInfoAutoPtr1(lEvent.splitData1.operator std::unique_ptr<pmSplitInfo>());
            std::unique_ptr<pmSplitInfo> lSplitInfoAutoPtr2(lEvent.splitData2.operator std::unique_ptr<pmSplitInfo>());

			DoSubtaskReduction(lEvent.task, lEvent.subtaskId1, lSplitInfoAutoPtr1.get(), lEvent.stub2, lEvent.subtaskId2, lSplitInfoAutoPtr2.get());

			break;
		}

        case NEGOTIATED_RANGE:
        {
            negotiatedRangeEvent& lEvent = static_cast<negotiatedRangeEvent&>(pEvent);

            DEBUG_EXCEPTION_ASSERT(lEvent.range.originalAllottee != NULL && lEvent.range.originalAllottee != GetProcessingElement());
        
            pmSubtaskRange& lRange = lEvent.range;
            for(ulong subtaskId = lRange.startSubtask; subtaskId <= lRange.endSubtask; ++subtaskId)
            {
            #ifdef DUMP_EVENT_TIMELINE
                mEventTimelineAutoPtr->RenameEvent(pmSubtaskRangeExecutionTimelineAutoPtr::GetCancelledEventName(subtaskId, lRange.task), pmSubtaskRangeExecutionTimelineAutoPtr::GetEventName(subtaskId, lRange.task));
            #endif

                CommonPostNegotiationOnCPU(lRange.task, subtaskId, true, NULL);
            }
        
        #ifdef TRACK_MULTI_ASSIGN
            if(lRange.task->GetSchedulingModel() == scheduler::PULL)
            {
                std::cout << "Multi assign partition [" << lRange.startSubtask << " - " << lRange.endSubtask << "] completed by secondary allottee - Device " << GetProcessingElement()->GetGlobalDeviceIndex() << ", Original Allottee: Device " << lEvent.range.originalAllottee->GetGlobalDeviceIndex() << std::endl;
            }
        #endif

            CommitRange(lRange, pmSuccess);
        
            break;
        }

		case FREE_GPU_RESOURCES:
		{
        #ifdef SUPPORT_CUDA
			((pmStubGPU*)this)->FreeExecutionResources();
        #endif

			break;
		}
        
        case POST_HANDLE_EXEC_COMPLETION:
        {
            execCompletionEvent& lEvent = static_cast<execCompletionEvent&>(pEvent);
            HandleRangeExecutionCompletion(lEvent.range, lEvent.execStatus);
        
            break;
        }
        
        case DEFERRED_SHADOW_MEM_COMMITS:
        {
            deferredShadowMemCommitsEvent& lEvent = static_cast<deferredShadowMemCommitsEvent&>(pEvent);
            pmTask* lTask = lEvent.task;

            std::vector<std::pair<ulong, pmSplitData> >::iterator lIter = mDeferredShadowMemCommits[lTask].begin();
            std::vector<std::pair<ulong, pmSplitData> >::iterator lEndIter = mDeferredShadowMemCommits[lTask].end();

            for(; lIter != lEndIter; ++lIter)
            {
                std::unique_ptr<pmSplitInfo> lAutoPtr((*lIter).second.operator std::unique_ptr<pmSplitInfo>());
                
                const std::vector<pmAddressSpace*>& lAddressSpaceVector = lTask->GetAddressSpaces();
                
                std::vector<pmAddressSpace*>::const_iterator lMemIter = lAddressSpaceVector.begin(), lMemEndIter = lAddressSpaceVector.end();
                for(uint lAddressSpaceIndex = 0; lMemIter != lMemEndIter; ++lMemIter, ++lAddressSpaceIndex)
                {
                    const pmAddressSpace* lAddressSpace = (*lMemIter);
                    
                    if(lTask->IsReadWrite(lAddressSpace) && !lTask->HasDisjointReadWritesAcrossSubtasks(lAddressSpace))
                        lTask->GetSubscriptionManager().CommitSubtaskShadowMem(this, (*lIter).first, lAutoPtr.get(), lAddressSpaceIndex);
                }
            }
        
            mDeferredShadowMemCommits.erase(lTask);
        
            lTask->RegisterStubShadowMemCommitMessage();
            
            break;
        }
        
        case REDUCTION_FINISH:
        {
            reductionFinishEvent& lEvent = static_cast<reductionFinishEvent&>(pEvent);
            pmTask* lTask = lEvent.task;

            lTask->GetReducer()->HandleReductionFinish();
            
            break;
        }
            
        case PROCESS_REDISTRIBUTION_BUCKET:
        {
            processRedistributionBucketEvent& lEvent = static_cast<processRedistributionBucketEvent&>(pEvent);
            pmTask* lTask = lEvent.task;
            
            lTask->GetRedistributor(lTask->GetAddressSpace(lEvent.addressSpaceIndex))->ProcessRedistributionBucket(lEvent.bucketIndex);
            
            break;
        }
            
        case FREE_TASK_RESOURCES:
        {
        #ifdef SUPPORT_CUDA
            freeTaskResourcesEvent& lEvent = static_cast<freeTaskResourcesEvent&>(pEvent);
            ((pmStubCUDA*)this)->FreeTaskResources(lEvent.taskOriginatingHost, lEvent.taskSequenceNumber);
        #endif
            
            break;
        }
            
    #ifdef SUPPORT_SPLIT_SUBTASKS
        case SPLIT_SUBTASK_CHECK:
        {
            splitSubtaskCheckEvent& lEvent = static_cast<splitSubtaskCheckEvent&>(pEvent);
            pmTask* lTask = lEvent.task;

            ExecutePendingSplit(lTask->GetSubtaskSplitter().GetPendingSplit(NULL, this), false);
            
            lTask->GetSubtaskSplitter().StubHasProcessedDummyEvent(this);
            
            break;
        }
    #endif
            
        case REMOTE_SUBTASK_REDUCE:
        {
            remoteSubtaskReduceEvent& lEvent = static_cast<remoteSubtaskReduceEvent&>(pEvent);
            
            pmTask* lTask = lEvent.task;
            communicator::subtaskReducePacked* lData = static_cast<communicator::subtaskReducePacked*>(lEvent.commandPtr->GetData());

            pmSubscriptionManager& lSubscriptionManager = lTask->GetSubscriptionManager();
            lSubscriptionManager.EraseSubtask(this, lData->reduceStruct.subtaskId, NULL);

            // Auto lock/unlock scope
            {
                guarded_ptr<RESOURCE_LOCK_IMPLEMENTATION_CLASS, currentSubtaskRangeStats> lScopedPtr(&mCurrentSubtaskRangeLock, &mCurrentSubtaskRangeStats, new currentSubtaskRangeStats(lTask, lData->reduceStruct.subtaskId, lData->reduceStruct.subtaskId, false, lData->reduceStruct.subtaskId, NULL, pmBase::GetCurrentTimeInSecs(), NULL, NULL));

                lSubscriptionManager.FindSubtaskMemDependencies(this, lData->reduceStruct.subtaskId, NULL);
            }

            std::vector<communicator::shadowMemTransferPacked>::iterator lShadowMemsIter = lData->shadowMems.begin();

            filtered_for_each_with_index(lTask->GetAddressSpaces(), [&] (const pmAddressSpace* pAddressSpace) {return (lTask->IsWritable(pAddressSpace) && lTask->IsReducible(pAddressSpace));},
            [&] (const pmAddressSpace* pAddressSpace, size_t pAddressSpaceIndex, size_t pOutputAddressSpaceIndex)
            {
                EXCEPTION_ASSERT(lShadowMemsIter != lData->shadowMems.end());

            #ifdef SUPPORT_LAZY_MEMORY
                if(lTask->IsLazyWriteOnly(pAddressSpace))
                {
                    uint lUnprotectedRanges = lShadowMemsIter->shadowMemData.writeOnlyUnprotectedPageRangesCount;
                    uint lUnprotectedLength = lUnprotectedRanges * 2 * sizeof(uint);
                    void* lMem = reinterpret_cast<void*>(reinterpret_cast<uint>(lShadowMemsIter->shadowMemData.subtaskMemLength) + lUnprotectedLength);

                    lSubscriptionManager.CreateSubtaskShadowMem(lStub, lData->reduceStruct.subtaskId, NULL, (uint)pAddressSpaceIndex, lMem, lShadowMemsIter->shadowMemData.subtaskMemLength - lUnprotectedLength, lUnprotectedRanges, (uint*)lShadowMemsIter->shadowMem.get_ptr());
                }
                else
            #endif
                {
                    std::cout << lShadowMemsIter->shadowMemData.subtaskMemLength << std::endl;
                    lSubscriptionManager.CreateSubtaskShadowMem(this, lData->reduceStruct.subtaskId, NULL, (uint)pAddressSpaceIndex, lShadowMemsIter->shadowMem.get_ptr(), lShadowMemsIter->shadowMemData.subtaskMemLength, 0, NULL);
                }
                
                ++lShadowMemsIter;
            });

            lTask->GetReducer()->AddSubtask(this, lData->reduceStruct.subtaskId, NULL);

            break;
        }
            
        default:
            PMTHROW(pmFatalErrorException());
	}
}
    
void pmExecutionStub::ExecuteSubtaskRange(execStub::subtaskExecEvent& pEvent)
{
    pmSubtaskRange& lRange = pEvent.range;
    pmSubtaskRange lCurrentRange(lRange);
    if(pEvent.rangeExecutedOnce)
        lCurrentRange.startSubtask = pEvent.lastExecutedSubtaskId + 1;

    DEBUG_EXCEPTION_ASSERT(lCurrentRange.startSubtask <= lCurrentRange.endSubtask);

    bool lPrematureTermination = false, lReassigned = false, lForceAckFlag = false;
    bool lIsMultiAssignRange = (lCurrentRange.task->IsMultiAssignEnabled() && lCurrentRange.originalAllottee != NULL);
    pmStatus lExecStatus = pmStatusUnavailable;

#ifdef DUMP_EVENT_TIMELINE
    // Timeline Scope
    {
        pmSubtaskRangeExecutionTimelineAutoPtr lRangeExecTimelineAutoPtr(lRange.task, mEventTimelineAutoPtr.get(), lCurrentRange.startSubtask, lCurrentRange.endSubtask);
        lCurrentRange.endSubtask = pmExecutionStub::ExecuteWrapper(lCurrentRange, pEvent, lIsMultiAssignRange, lRangeExecTimelineAutoPtr, lReassigned, lForceAckFlag, lPrematureTermination, lExecStatus);
        
        if(!lPrematureTermination && !lReassigned && !lIsMultiAssignRange)
            lRangeExecTimelineAutoPtr.SetGracefulCompletion();
    }
#else
    lCurrentRange.endSubtask = pmExecutionStub::ExecuteWrapper(lCurrentRange, pEvent, lIsMultiAssignRange, lReassigned, lForceAckFlag, lPrematureTermination, lExecStatus);
#endif

    DEBUG_EXCEPTION_ASSERT(lCurrentRange.startSubtask <= lCurrentRange.endSubtask);

    if(lReassigned) // A secondary allottee has finished and negotiated this subtask range and added a POST_HANDLE_EXEC_COMPLETION for the rest of the range
        return;

    if(lPrematureTermination)
    {
    #ifdef TRACK_SUBTASK_EXECUTION_VERBOSE
        std::cout << "[Host " << pmGetHostId() << "]: Prematurely terminated subtask range [" << lCurrentRange.startSubtask << " - " << lCurrentRange.endSubtask << "]" << std::endl;
    #endif
    
        if(lForceAckFlag && lCurrentRange.endSubtask != 0)
        {
            lRange.endSubtask = lCurrentRange.endSubtask - 1;
        
            if(lRange.endSubtask >= lRange.startSubtask)
                HandleRangeExecutionCompletion(lRange, pmSuccess);
        }
    }
    else
    {
    #ifdef TRACK_SUBTASK_EXECUTION_VERBOSE
        std::cout << "[Host " << pmGetHostId() << "]: Executed subtask range [" << lCurrentRange.startSubtask << " - " << lCurrentRange.endSubtask << "] - " << lRange.endSubtask << std::endl;
    #endif
    
        if(lRange.originalAllottee == NULL)
        {
            for(ulong lSubtaskId = lCurrentRange.startSubtask; lSubtaskId <= lCurrentRange.endSubtask; ++lSubtaskId)
                CommonPostNegotiationOnCPU(lRange.task, lSubtaskId, false, NULL);
        }

        if(lForceAckFlag)
            lRange.endSubtask = lCurrentRange.endSubtask;
    
        if(lCurrentRange.endSubtask == lRange.endSubtask)
            HandleRangeExecutionCompletion(lRange, lExecStatus);
    }
}

void pmExecutionStub::ClearSecondaryAllotteeMap(pmSubtaskRange& pRange)
{
    if(pRange.task->GetSchedulingModel() != scheduler::PULL)
        return;
        
    FINALIZE_RESOURCE_PTR(dSecondaryAllotteeLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mSecondaryAllotteeLock, Lock(), Unlock());

    std::pair<pmTask*, ulong> lPair(pRange.task, pRange.endSubtask);
    
    auto lIter = mSecondaryAllotteeMap.find(lPair);
    if(lIter != mSecondaryAllotteeMap.end())
    {
        std::vector<const pmProcessingElement*>& lSecondaryAllottees = lIter->second;

        for_each(lSecondaryAllottees, [&pRange] (const pmProcessingElement* pProcessingElement)
        {
            pmScheduler::GetScheduler()->SendSubtaskRangeCancellationMessage(pProcessingElement, pRange);
        });

    #ifdef TRACK_MULTI_ASSIGN
        std::cout << "Multi assign partition [" << pRange.startSubtask << " - " << pRange.endSubtask << "] completed by original allottee - Device " << GetProcessingElement()->GetGlobalDeviceIndex() << std::endl;
    #endif

        mSecondaryAllotteeMap.erase(lIter);
    }
}

void pmExecutionStub::HandleRangeExecutionCompletion(pmSubtaskRange& pRange, pmStatus pExecStatus)
{
    if(pRange.task->IsMultiAssignEnabled())
    {
        if(pRange.originalAllottee != NULL)
        {
            // All secondary allottees must get go ahead from original allottee
            pmScheduler::GetScheduler()->NegotiateSubtaskRangeWithOriginalAllottee(GetProcessingElement(), pRange);
            return;
        }

        ClearSecondaryAllotteeMap(pRange);
    }

    CommitRange(pRange, pExecStatus);
}

void pmExecutionStub::CommitRange(pmSubtaskRange& pRange, pmStatus pExecStatus)
{
    std::vector<communicator::ownershipDataStruct> lOwnershipVector;
    std::vector<uint> lAddressSpaceIndexVector;

    if(!pRange.task->GetCallbackUnit()->GetDataReductionCB() && !pRange.task->GetCallbackUnit()->GetDataRedistributionCB())
    {
        pmSubscriptionManager& lSubscriptionManager = pRange.task->GetSubscriptionManager();
        
        filtered_for_each_with_index(pRange.task->GetAddressSpaces(), [&pRange] (const pmAddressSpace* pAddressSpace) {return pRange.task->IsWritable(pAddressSpace);},
        [&] (const pmAddressSpace* pAddressSpace, size_t pAddressSpaceIndex, size_t pOutputAddressSpaceIndex)
        {
            subscription::subscriptionRecordType::const_iterator lBeginIter, lEndIter;
            lAddressSpaceIndexVector.push_back((uint)lOwnershipVector.size());

            for(ulong lSubtaskId = pRange.startSubtask; lSubtaskId <= pRange.endSubtask; ++lSubtaskId)
            {
                lSubscriptionManager.GetNonConsolidatedWriteSubscriptions(this, lSubtaskId, NULL, (uint)pAddressSpaceIndex, lBeginIter, lEndIter);
                
                std::for_each(lBeginIter, lEndIter, [&lOwnershipVector] (const decltype(lBeginIter)::value_type& pPair)
                {
                    lOwnershipVector.push_back(communicator::ownershipDataStruct(pPair.first, pPair.second.first));
                });
            }
        });
    }

    pmScheduler::GetScheduler()->SendAcknowledgement(GetProcessingElement(), pRange, pExecStatus, std::move(lOwnershipVector), std::move(lAddressSpaceIndexVector));
}

#ifdef SUPPORT_SPLIT_SUBTASKS
bool pmExecutionStub::UpdateSecondaryAllotteeMap(std::pair<pmTask*, ulong>& pPair, const pmProcessingElement* pRequestingDevice)
{
    FINALIZE_RESOURCE_PTR(dSecondaryAllotteeLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mSecondaryAllotteeLock, Lock(), Unlock());

    return UpdateSecondaryAllotteeMapInternal(pPair, pRequestingDevice);
}

/* This method must be called with mCurrentSubtaskRangeLock acquired */
bool pmExecutionStub::UpdateSecondaryAllotteeMapInternal(std::pair<pmTask*, ulong>& pPair, const pmProcessingElement* pRequestingDevice)
{
    DEBUG_EXCEPTION_ASSERT(pPair.first->GetSchedulingModel() == scheduler::PULL);

    std::map<std::pair<pmTask*, ulong>, std::vector<const pmProcessingElement*> >::iterator lIter = mSecondaryAllotteeMap.find(pPair);
    if(lIter != mSecondaryAllotteeMap.end())
    {
        std::vector<const pmProcessingElement*>::iterator lInnerIter = lIter->second.begin(), lInnerEndIter = lIter->second.end();
        for(; lInnerIter != lInnerEndIter; ++lInnerIter)
        {
            if((*lInnerIter) == pRequestingDevice)
                return false;
        }
    }
    
    mSecondaryAllotteeMap[pPair].push_back(pRequestingDevice);
    
    return true;
}
    
/* This method is called on the stub object of the stub to which the original unsplitted subtask was assigned.
 But it is called on the thread for the last stub finishing it's assigned split. */
void pmExecutionStub::HandleSplitSubtaskExecutionCompletion(pmTask* pTask, const splitter::splitRecord& pSplitRecord, pmStatus pExecStatus)
{
    pmSubtaskRange lRange(pTask, NULL, pSplitRecord.subtaskId, pSplitRecord.subtaskId);
    
    if(pTask->IsMultiAssignEnabled())
        ClearSecondaryAllotteeMap(lRange);

    CommitSplitSubtask(lRange, pSplitRecord, pExecStatus);
}
    
void pmExecutionStub::CommitSplitSubtask(pmSubtaskRange& pRange, const splitter::splitRecord& pSplitRecord, pmStatus pExecStatus)
{
    EXCEPTION_ASSERT(pRange.startSubtask == pRange.endSubtask && pRange.endSubtask == pSplitRecord.subtaskId);

    // PUSH expects one consolidated acknowledgement for the entire assigned range
    if(pRange.task->GetSchedulingModel() == scheduler::PUSH && !pRange.originalAllottee && pRange.task->GetSubtaskSplitter().IsSplitting(GetType()))
    {
        FINALIZE_RESOURCE_PTR(dPushAckLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mPushAckLock, Lock(), Unlock());

        std::map<pmTask*, std::pair<std::pair<ulong, ulong>, std::map<ulong, std::vector<pmExecutionStub*> > > >::iterator lIter = mPushAckHolder.find(pRange.task);

        if(lIter == mPushAckHolder.end())
            return; // Probably negotiated

        DEBUG_EXCEPTION_ASSERT(lIter->second.second.find(pSplitRecord.subtaskId) == lIter->second.second.end());
        
        std::map<ulong, std::vector<pmExecutionStub*> >::iterator lMapIter = lIter->second.second.emplace(std::piecewise_construct, std::forward_as_tuple(pSplitRecord.subtaskId), std::forward_as_tuple()).first;
        lMapIter->second.reserve(pSplitRecord.splitCount);

        for(uint i = 0; i < pSplitRecord.splitCount; ++i)
            lMapIter->second.push_back(pSplitRecord.assignedStubs[i].first);

        ulong lSubtasks = lIter->second.first.second - lIter->second.first.first + 1;
        
        DEBUG_EXCEPTION_ASSERT(lIter->second.second.size() <= lSubtasks);

        if(lIter->second.second.size() != lSubtasks)
            return;

        pmSubtaskRange lRange(pRange.task, NULL, lIter->second.first.first, lIter->second.first.second);
        SendSplitAcknowledgement(lRange, lIter->second.second, pExecStatus);

        mPushAckHolder.erase(lIter);
    }
    else
    {
        std::map<ulong, std::vector<pmExecutionStub*> > lMap;

        std::map<ulong, std::vector<pmExecutionStub*> >::iterator lMapIter = lMap.emplace(std::piecewise_construct, std::forward_as_tuple(pSplitRecord.subtaskId), std::forward_as_tuple()).first;
        lMapIter->second.reserve(pSplitRecord.splitCount);
    
        for(uint i = 0; i < pSplitRecord.splitCount; ++i)
            lMapIter->second.push_back(pSplitRecord.assignedStubs[i].first);

        SendSplitAcknowledgement(pRange, lMap, pExecStatus);
    }
}
    
void pmExecutionStub::SendSplitAcknowledgement(const pmSubtaskRange& pRange, const std::map<ulong, std::vector<pmExecutionStub*> >& pMap, pmStatus pExecStatus)
{
    std::vector<communicator::ownershipDataStruct> lOwnershipVector;
    std::vector<uint> lAddressSpaceIndexVector;

    if(!pRange.task->GetCallbackUnit()->GetDataReductionCB() && !pRange.task->GetCallbackUnit()->GetDataRedistributionCB())
    {
        subscription::subscriptionRecordType::const_iterator lBeginIter, lEndIter;
        pmSubscriptionManager& lSubscriptionManager = pRange.task->GetSubscriptionManager();
        std::vector<pmAddressSpace*>& lAddressSpaceVector = pRange.task->GetAddressSpaces();

        filtered_for_each_with_index(lAddressSpaceVector, [&pRange] (const pmAddressSpace* pAddressSpace) {return pRange.task->IsWritable(pAddressSpace);},
        [&] (const pmAddressSpace* pAddressSpace, size_t pAddressSpaceIndex, size_t pOutputAddressSpaceIndex)
        {
            lAddressSpaceIndexVector.push_back((uint)lOwnershipVector.size());

            for(ulong lSubtaskId = pRange.startSubtask; lSubtaskId <= pRange.endSubtask; ++lSubtaskId)
            {
                const std::vector<pmExecutionStub*>& lVector = pMap.find(lSubtaskId)->second;
                uint lSplitCount = (uint)lVector.size();

                for_each_with_index(lVector, [&] (const pmExecutionStub* pStub, size_t pSplitIndex)
                {
                    pmSplitInfo lSplitInfo((uint)pSplitIndex, lSplitCount);
                    lSubscriptionManager.GetNonConsolidatedWriteSubscriptions(pStub, lSubtaskId, &lSplitInfo, (uint)pAddressSpaceIndex, lBeginIter, lEndIter);

                    std::for_each(lBeginIter, lEndIter, [&lOwnershipVector] (const decltype(lBeginIter)::value_type& pPair)
                    {
                        lOwnershipVector.push_back(communicator::ownershipDataStruct(pPair.first, pPair.second.first));
                    });
                });
            }
        });
    }

    pmScheduler::GetScheduler()->SendAcknowledgement(GetProcessingElement(), pRange, pExecStatus, std::move(lOwnershipVector), std::move(lAddressSpaceIndexVector));
}

bool pmExecutionStub::CheckSplittedExecution(subtaskExecEvent& pEvent)
{
    pmSubtaskRange& lRange = pEvent.range;

    pmSubtaskSplitter& lSubtaskSplitter = lRange.task->GetSubtaskSplitter();
    if(!lSubtaskSplitter.IsSplitting(GetType()))
        return false;

    bool lIsMultiAssignRange = (lRange.task->IsMultiAssignEnabled() && lRange.originalAllottee != NULL);
    EXCEPTION_ASSERT(!lIsMultiAssignRange);

    ulong lSubtaskId = ((pEvent.rangeExecutedOnce) ? (pEvent.lastExecutedSubtaskId + 1) : lRange.startSubtask);

    std::unique_ptr<pmSplitSubtask> lSplitSubtaskAutoPtr = lSubtaskSplitter.GetPendingSplit(&lSubtaskId, this);
    if(!lSplitSubtaskAutoPtr.get())
        return false;

    if(lSplitSubtaskAutoPtr->subtaskId == lSubtaskId)
    {
        // Remove the split subtask from the range in the current event, so that it does not send it's acknowledgement later
        if(lSubtaskId != lRange.startSubtask)
        {
            pmSubtaskRange lFinishedRange(lRange.task, lRange.originalAllottee, lRange.startSubtask, lSubtaskId - 1);
            HandleRangeExecutionCompletion(lFinishedRange, pmSuccess);
        }
        
        if(lSubtaskId < lRange.endSubtask)
        {
            SwitchThread(std::shared_ptr<stubEvent>(new subtaskExecEvent(SUBTASK_EXEC, pmSubtaskRange(pEvent.range.task, pEvent.range.originalAllottee, lSubtaskId + 1, pEvent.range.endSubtask), false, 0)), lRange.task->GetPriority());
        }
    }
    else    // Push back current event in the queue as some other split subtask has been assigned
    {
        SwitchThread(std::shared_ptr<stubEvent>(new subtaskExecEvent(SUBTASK_EXEC, pEvent.range, pEvent.rangeExecutedOnce, pEvent.lastExecutedSubtaskId)), lRange.task->GetPriority());
    }
    
    ExecutePendingSplit(std::move(lSplitSubtaskAutoPtr), true);
    
    return true;
}
    
void pmExecutionStub::ExecutePendingSplit(std::unique_ptr<pmSplitSubtask>&& pSplitSubtaskAutoPtr, bool pSecondaryOperationsBlocked)
{
    if(!pSplitSubtaskAutoPtr.get())
        return;

#ifdef DUMP_EVENT_TIMELINE
    pmSplitSubtaskExecutionTimelineAutoPtr lExecTimelineAutoPtr(pSplitSubtaskAutoPtr->task, mEventTimelineAutoPtr.get(), pSplitSubtaskAutoPtr->subtaskId, pSplitSubtaskAutoPtr->splitId, pSplitSubtaskAutoPtr->splitCount);
#endif

    bool lMultiAssign = false, lPrematureTermination = false, lReassigned = false, lForceAckFlag = false;
    ExecuteSplitSubtask(pSplitSubtaskAutoPtr, pSecondaryOperationsBlocked, lMultiAssign, lPrematureTermination, lReassigned, lForceAckFlag);

    if(lReassigned)
        return;
    
    if(lPrematureTermination)
    {
    #ifdef TRACK_SUBTASK_EXECUTION_VERBOSE
        std::cout << "[Host " << pmGetHostId() << "]: Prematurely terminated split subtask " << pSplitSubtaskAutoPtr->subtaskId << " (Split " << pSplitSubtaskAutoPtr->splitId << " of " << pSplitSubtaskAutoPtr->splitCount << ")" << std::endl;
    #endif
    }
    else
    {
    #ifdef DUMP_EVENT_TIMELINE
        lExecTimelineAutoPtr.SetGracefulCompletion();
    #endif

    #ifdef TRACK_SUBTASK_EXECUTION_VERBOSE
        std::cout << "[Host " << pmGetHostId() << "]: Executed split subtask " << pSplitSubtaskAutoPtr->subtaskId << " (Split " << pSplitSubtaskAutoPtr->splitId << " of " << pSplitSubtaskAutoPtr->splitCount << ")" << std::endl;
    #endif
    }

    pSplitSubtaskAutoPtr->task->GetSubtaskSplitter().FinishedSplitExecution(pSplitSubtaskAutoPtr->subtaskId, pSplitSubtaskAutoPtr->splitId, this, lPrematureTermination);
}
    
void pmExecutionStub::ExecuteSplitSubtask(const std::unique_ptr<pmSplitSubtask>& pSplitSubtaskAutoPtr, bool pSecondaryOperationsBlocked, bool pMultiAssign, bool& pPrematureTermination, bool& pReassigned, bool& pForceAckFlag)
{
    currentSubtaskRangeTerminus lTerminus(pReassigned, pForceAckFlag, pPrematureTermination, this);

    ulong lSubtaskId = pSplitSubtaskAutoPtr->subtaskId;

    pmSplitInfo lSplitInfo(pSplitSubtaskAutoPtr->splitId, pSplitSubtaskAutoPtr->splitCount);

    guarded_scoped_ptr<RESOURCE_LOCK_IMPLEMENTATION_CLASS, currentSubtaskRangeTerminus, currentSubtaskRangeStats> lScopedPtr(&mCurrentSubtaskRangeLock, &lTerminus, &mCurrentSubtaskRangeStats, new currentSubtaskRangeStats(pSplitSubtaskAutoPtr->task, lSubtaskId, lSubtaskId, !pMultiAssign, lSubtaskId, NULL, pmBase::GetCurrentTimeInSecs(), &lSplitInfo, pSplitSubtaskAutoPtr->sourceStub));
    
#ifdef ENABLE_TASK_PROFILING
    pmRecordProfileEventAutoPtr lRecordProfileEventAutoPtr(pSplitSubtaskAutoPtr->task->GetTaskProfiler(), taskProfiler::SUBTASK_EXECUTION);
#endif

    try
    {
        if(pSecondaryOperationsBlocked)
            UnblockSecondaryCommands(); // Allow external operations (steal & range negotiation) on priority queue
        
        TIMER_IMPLEMENTATION_CLASS lTimer;
        lTimer.Start();

        pmSubtaskRange lCurrentRange(pSplitSubtaskAutoPtr->task, NULL, lSubtaskId, lSubtaskId);
        ulong lEndSubtask = FindCollectivelyExecutableSubtaskRangeEnd(lCurrentRange, &lSplitInfo, pMultiAssign);

        EXCEPTION_ASSERT(lEndSubtask == lCurrentRange.startSubtask && lEndSubtask == lCurrentRange.endSubtask);

        PrepareForSubtaskRangeExecution(pSplitSubtaskAutoPtr->task, lSubtaskId, lSubtaskId, &lSplitInfo);

        {
            FINALIZE_RESOURCE_PTR(dCurrentSubtaskLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCurrentSubtaskRangeLock, Lock(), Unlock());
            
            mCurrentSubtaskRangeStats->currentSubtaskId = lEndSubtask;
            mCurrentSubtaskRangeStats->currentSubtaskIdValid = true;
            mCurrentSubtaskRangeStats->currentSubtaskInPostDataFetchStage = false;
        }

        TLS_IMPLEMENTATION_CLASS::GetTls()->SetThreadLocalStorage(TLS_CURRENT_SUBTASK_ID, &lSubtaskId);

    #ifdef SUPPORT_LAZY_MEMORY
        TLS_IMPLEMENTATION_CLASS::GetTls()->SetThreadLocalStorage(TLS_SPLIT_ID, &lSplitInfo.splitId);
        TLS_IMPLEMENTATION_CLASS::GetTls()->SetThreadLocalStorage(TLS_SPLIT_COUNT, &lSplitInfo.splitCount);
    #endif
        
        Execute(pSplitSubtaskAutoPtr->task, lSubtaskId, pMultiAssign, NULL, &lSplitInfo);
        
        TLS_IMPLEMENTATION_CLASS::GetTls()->SetThreadLocalStorage(TLS_CURRENT_SUBTASK_ID, NULL);

    #ifdef SUPPORT_LAZY_MEMORY
        TLS_IMPLEMENTATION_CLASS::GetTls()->SetThreadLocalStorage(TLS_SPLIT_ID, NULL);
        TLS_IMPLEMENTATION_CLASS::GetTls()->SetThreadLocalStorage(TLS_SPLIT_COUNT, NULL);
    #endif

        WaitForSubtaskExecutionToFinish(pSplitSubtaskAutoPtr->task, lSubtaskId, lSubtaskId, &lSplitInfo);
        
        lTimer.Stop();

        pSplitSubtaskAutoPtr->task->GetTaskExecStats().RecordStubExecutionStats(this, 1, lTimer.GetElapsedTimeInSecs());   // Exec time of the split group
    }
    catch(pmPrematureExitException& e)
    {
        if(e.IsSubtaskLockAcquired())
            lScopedPtr.SetLockAcquired();
    }
    catch(...)
    {
        CleanupPostSubtaskRangeExecution(pSplitSubtaskAutoPtr->task, lSubtaskId, lSubtaskId, lSubtaskId, &lSplitInfo);
        throw;
    }

    CleanupPostSubtaskRangeExecution(pSplitSubtaskAutoPtr->task, lSubtaskId, lSubtaskId, lSubtaskId, &lSplitInfo);
}
#endif

void pmExecutionStub::DeferShadowMemCommit(pmTask* pTask, ulong pSubtaskId, pmSplitInfo* pSplitInfo)
{
    pmSplitData lSplitData(false, 0, 0);
    pmSplitData::ConvertSplitInfoToSplitData(lSplitData, pSplitInfo);

    mDeferredShadowMemCommits[pTask].push_back(std::make_pair(pSubtaskId, lSplitData));
}

// This method must be called with mCurrentSubtaskRangeLock acquired
void pmExecutionStub::CancelCurrentlyExecutingSubtaskRange(bool pTaskListeningOnCancellation)
{
    DEBUG_EXCEPTION_ASSERT(mCurrentSubtaskRangeStats);

    if(!mCurrentSubtaskRangeStats->taskListeningOnCancellation && pTaskListeningOnCancellation)
        mCurrentSubtaskRangeStats->task->RecordStubWillSendCancellationMessage();

    mCurrentSubtaskRangeStats->taskListeningOnCancellation |= pTaskListeningOnCancellation;
    
    if(!mCurrentSubtaskRangeStats->prematureTermination)
    {
        mCurrentSubtaskRangeStats->prematureTermination = true;
        
        if(!mExecutingLibraryCode)
        {
            if(mCurrentSubtaskRangeStats->task->CanForciblyCancelSubtasks())
                TerminateUserModeExecution();
        }
        else
        {
            if(mCurrentSubtaskRangeStats->accumulatorCommandPtr)
                (static_cast<pmAccumulatorCommand*>((*mCurrentSubtaskRangeStats->accumulatorCommandPtr).get()))->ForceComplete(*mCurrentSubtaskRangeStats->accumulatorCommandPtr);
        }
    }
}

void pmExecutionStub::MarkInsideLibraryCode()
{
    mExecutingLibraryCode = 1;

    FINALIZE_RESOURCE_PTR(dCurrentSubtaskLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCurrentSubtaskRangeLock, Lock(), Unlock());

    CheckTermination();
}

void pmExecutionStub::MarkInsideUserCode()
{
    // Auto lock/unlock scope
    {
        FINALIZE_RESOURCE_PTR(dCurrentSubtaskLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCurrentSubtaskRangeLock, Lock(), Unlock());

        CheckTermination();
    }
    
    mExecutingLibraryCode = 0;
}
    
void pmExecutionStub::SetupJmpBuf(sigjmp_buf* pJmpBuf)
{
    // Auto lock/unlock scope
    {
        FINALIZE_RESOURCE_PTR(dCurrentSubtaskLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCurrentSubtaskRangeLock, Lock(), Unlock());

        if(mCurrentSubtaskRangeStats->jmpBuf)
            PMTHROW(pmFatalErrorException());
        
        mCurrentSubtaskRangeStats->jmpBuf = pJmpBuf;

        CheckTermination();
    }
    
    mExecutingLibraryCode = 0;
}
    
void pmExecutionStub::UnsetupJmpBuf(bool pHasJumped)
{
    mExecutingLibraryCode = 1;

    if(pHasJumped)
    {
        if(!mCurrentSubtaskRangeStats->jmpBuf)
            PMTHROW(pmFatalErrorException());
        
        mCurrentSubtaskRangeStats->jmpBuf = NULL;
    }
    else
    {
        FINALIZE_RESOURCE_PTR(dCurrentSubtaskLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCurrentSubtaskRangeLock, Lock(), Unlock());

        if(!mCurrentSubtaskRangeStats->jmpBuf)
            PMTHROW(pmFatalErrorException());
        
        mCurrentSubtaskRangeStats->jmpBuf = NULL;
    }
}

// This method must be called with mCurrentSubtaskRangeLock acquired
void pmExecutionStub::CheckTermination()
{
    if(!mCurrentSubtaskRangeStats)
        return;

    if(mCurrentSubtaskRangeStats->prematureTermination)
        TerminateCurrentSubtaskRange();
}

// This method must be called with mCurrentSubtaskRangeLock acquired
void pmExecutionStub::TerminateCurrentSubtaskRange()
{
    DEBUG_EXCEPTION_ASSERT(mCurrentSubtaskRangeStats->jmpBuf);

    mExecutingLibraryCode = 1;
    siglongjmp(*(mCurrentSubtaskRangeStats->jmpBuf), 1);
}

bool pmExecutionStub::RequiresPrematureExit()
{
    FINALIZE_RESOURCE_PTR(dCurrentSubtaskLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCurrentSubtaskRangeLock, Lock(), Unlock());

    DEBUG_EXCEPTION_ASSERT(mCurrentSubtaskRangeStats);

    return mCurrentSubtaskRangeStats->prematureTermination;
}

bool pmExecutionStub::IsHighPriorityEventWaiting(ushort pPriority)
{
	return GetPriorityQueue().IsHighPriorityElementPresent(pPriority);
}

#ifdef DUMP_EVENT_TIMELINE
ulong pmExecutionStub::ExecuteWrapper(const pmSubtaskRange& pCurrentRange, const subtaskExecEvent& pEvent, bool pIsMultiAssign, pmSubtaskRangeExecutionTimelineAutoPtr& pRangeExecTimelineAutoPtr, bool& pReassigned, bool& pForceAckFlag, bool& pPrematureTermination, pmStatus& pStatus)
#else
ulong pmExecutionStub::ExecuteWrapper(const pmSubtaskRange& pCurrentRange, const subtaskExecEvent& pEvent, bool pIsMultiAssign, bool& pReassigned, bool& pForceAckFlag, bool& pPrematureTermination, pmStatus& pStatus)
#endif
{
    ulong lStartSubtask = pCurrentRange.startSubtask;
    ulong lEndSubtask = std::numeric_limits<ulong>::max();
    ulong lCleanupEndSubtask = lEndSubtask;

    const pmSubtaskRange& lParentRange = pEvent.range;

    currentSubtaskRangeTerminus lTerminus(pReassigned, pForceAckFlag, pPrematureTermination, this);

    guarded_scoped_ptr<RESOURCE_LOCK_IMPLEMENTATION_CLASS, currentSubtaskRangeTerminus, currentSubtaskRangeStats> lScopedPtr(&mCurrentSubtaskRangeLock, &lTerminus, &mCurrentSubtaskRangeStats, new currentSubtaskRangeStats(pCurrentRange.task, pCurrentRange.startSubtask, pCurrentRange.endSubtask, !pIsMultiAssign, lParentRange.startSubtask, NULL, pmBase::GetCurrentTimeInSecs(), NULL, NULL));
    
#ifdef ENABLE_TASK_PROFILING
    pmRecordProfileEventAutoPtr lRecordProfileEventAutoPtr(pCurrentRange.task->GetTaskProfiler(), taskProfiler::SUBTASK_EXECUTION);
#endif

    try
    {
        TIMER_IMPLEMENTATION_CLASS lCommonTimer;
        lCommonTimer.Start();

        lCleanupEndSubtask = lEndSubtask = FindCollectivelyExecutableSubtaskRangeEnd(pCurrentRange, NULL, pIsMultiAssign);

        EXCEPTION_ASSERT(lEndSubtask >= pCurrentRange.startSubtask && lEndSubtask <= pCurrentRange.endSubtask);
        
        if(lEndSubtask != pCurrentRange.endSubtask)
        {
        #ifdef DUMP_EVENT_TIMELINE
            pRangeExecTimelineAutoPtr.ResetEndSubtask(lEndSubtask);
        #endif
            
            FINALIZE_RESOURCE_PTR(dCurrentSubtaskLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCurrentSubtaskRangeLock, Lock(), Unlock());
            mCurrentSubtaskRangeStats->ResetEndSubtaskId(lEndSubtask);
        }

        if(lEndSubtask != lParentRange.endSubtask)
            SwitchThread(std::shared_ptr<stubEvent>(new subtaskExecEvent(SUBTASK_EXEC, pEvent.range, true, lEndSubtask)), lParentRange.task->GetPriority());

        ulong* lPrefetchSubtaskIdPtr = NULL;

    #ifdef SUPPORT_COMPUTE_COMMUNICATION_OVERLAP
        ulong lPrefetchSubtaskId = std::numeric_limits<ulong>::max();
        if(pCurrentRange.task->ShouldOverlapComputeCommunication() && lEndSubtask != lParentRange.endSubtask)
        {
            lPrefetchSubtaskIdPtr = &lPrefetchSubtaskId;
            lPrefetchSubtaskId = lEndSubtask + 1;
        }
    #endif

        UnblockSecondaryCommands(); // Allow external operations (steal & range negotiation) on priority queue

        PrepareForSubtaskRangeExecution(pCurrentRange.task, lStartSubtask, lEndSubtask, NULL);
        
        lCommonTimer.Stop();

        ulong lSubtaskCount = (lEndSubtask - lStartSubtask + 1);
        ulong lCommonTimePerSubtask = (lCommonTimer.GetElapsedTimeInSecs() / lSubtaskCount);
        
        std::vector<TIMER_IMPLEMENTATION_CLASS> lSubtaskTimerVector;
        lSubtaskTimerVector.resize(lSubtaskCount);
        
        for(ulong lSubtaskId = lStartSubtask; lSubtaskId <= lEndSubtask; ++lSubtaskId)
        {
            // Auto lock/unlock scope
            {
                FINALIZE_RESOURCE_PTR(dCurrentSubtaskLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCurrentSubtaskRangeLock, Lock(), Unlock());

                if(mCurrentSubtaskRangeStats->endSubtaskId != lEndSubtask)  // A steal can happen from the end
                {
                    DEBUG_EXCEPTION_ASSERT(mCurrentSubtaskRangeStats->endSubtaskId >= lSubtaskId - 1);
                    lEndSubtask = mCurrentSubtaskRangeStats->endSubtaskId;

                    DEBUG_EXCEPTION_ASSERT(lEndSubtask >= lStartSubtask);
                    if(lEndSubtask < lSubtaskId)
                        break;
                }
                
                mCurrentSubtaskRangeStats->currentSubtaskId = lSubtaskId;
                mCurrentSubtaskRangeStats->currentSubtaskIdValid = true;
                mCurrentSubtaskRangeStats->currentSubtaskInPostDataFetchStage = false;
            }
            
            TIMER_IMPLEMENTATION_CLASS& lTimer = lSubtaskTimerVector[lSubtaskId - lStartSubtask];
            lTimer.Start();
            
            TLS_IMPLEMENTATION_CLASS::GetTls()->SetThreadLocalStorage(TLS_CURRENT_SUBTASK_ID, &lSubtaskId);

        #ifdef SUPPORT_LAZY_MEMORY
            TLS_IMPLEMENTATION_CLASS::GetTls()->SetThreadLocalStorage(TLS_SPLIT_ID, NULL);
            TLS_IMPLEMENTATION_CLASS::GetTls()->SetThreadLocalStorage(TLS_SPLIT_COUNT, NULL);
        #endif
            
        #ifdef DUMP_EVENT_TIMELINE
            pRangeExecTimelineAutoPtr.InitializeNextSubtask();
        #endif
            
            Execute(pCurrentRange.task, lSubtaskId, pIsMultiAssign, lPrefetchSubtaskIdPtr, NULL);
            
            TLS_IMPLEMENTATION_CLASS::GetTls()->SetThreadLocalStorage(TLS_CURRENT_SUBTASK_ID, NULL);

            lTimer.Pause();
        }

        for(ulong lSubtaskId = lStartSubtask; lSubtaskId <= lEndSubtask; ++lSubtaskId)
        {
            TIMER_IMPLEMENTATION_CLASS& lTimer = lSubtaskTimerVector[lSubtaskId - lStartSubtask];
            lTimer.Resume();
            
            WaitForSubtaskExecutionToFinish(pCurrentRange.task, lStartSubtask, lSubtaskId, NULL);
        
            lTimer.Stop();

            pCurrentRange.task->GetTaskExecStats().RecordStubExecutionStats(this, 1, lTimer.GetElapsedTimeInSecs() + lCommonTimePerSubtask);
        }
    }
    catch(pmPrematureExitException& e)
    {
        if(e.IsSubtaskLockAcquired())
            lScopedPtr.SetLockAcquired();
    }
    catch(...)
    {
        CleanupPostSubtaskRangeExecution(pCurrentRange.task, lStartSubtask, lEndSubtask, lCleanupEndSubtask, NULL);
        throw;
    }

    CleanupPostSubtaskRangeExecution(pCurrentRange.task, lStartSubtask, lEndSubtask, lCleanupEndSubtask, NULL);

    return lEndSubtask;
}
    
void pmExecutionStub::CommonPreExecuteOnCPU(pmTask* pTask, ulong pSubtaskId, bool pIsMultiAssign, bool pPrefetch, pmSplitInfo* pSplitInfo)
{
    pmSubscriptionManager& lSubscriptionManager = pTask->GetSubscriptionManager();

#ifdef SUPPORT_SPLIT_SUBTASKS
    if(pSplitInfo)
        pTask->GetSubtaskSplitter().PrefetchSubscriptionsForUnsplittedSubtask(this, pSubtaskId);
#endif

    lSubscriptionManager.FindSubtaskMemDependencies(this, pSubtaskId, pSplitInfo);
    lSubscriptionManager.FetchSubtaskSubscriptions(this, pSubtaskId, pSplitInfo, GetType(), pPrefetch);
    
    if(!pPrefetch)
    {
        for_each_with_index(pTask->GetAddressSpaces(), [&] (const pmAddressSpace* pAddressSpace, size_t pAddressSpaceIndex)
        {
            if(pTask->IsWritable(pAddressSpace))
            {
                if(pTask->DoSubtasksNeedShadowMemory(pAddressSpace) || (pTask->IsMultiAssignEnabled() && pIsMultiAssign))
                    lSubscriptionManager.CreateSubtaskShadowMem(this, pSubtaskId, pSplitInfo, (uint)pAddressSpaceIndex);
            }
        });
    }
    
    FINALIZE_RESOURCE_PTR(dCurrentSubtaskLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCurrentSubtaskRangeLock, Lock(), Unlock());
    mCurrentSubtaskRangeStats->currentSubtaskInPostDataFetchStage = true;
}

void pmExecutionStub::CommonPostNegotiationOnCPU(pmTask* pTask, ulong pSubtaskId, bool pIsMultiAssign, pmSplitInfo* pSplitInfo)
{
	const pmCallbackUnit* lCallbackUnit = pTask->GetCallbackUnit();
	const pmDataReductionCB* lReduceCallback = lCallbackUnit->GetDataReductionCB();
    
	if(lReduceCallback)
		pTask->GetReducer()->AddSubtask(this, pSubtaskId, pSplitInfo);
	else
		INVOKE_SAFE_THROW_ON_FAILURE(pmDataRedistributionCB, pTask->GetCallbackUnit()->GetDataRedistributionCB(), Invoke, this, pTask, pSubtaskId, pSplitInfo, pIsMultiAssign);

    bool lDeferCommit = false;

    if(!lReduceCallback)
    {
        pmSubscriptionManager& lSubscriptionManager = pTask->GetSubscriptionManager();
        
        for_each_with_index(pTask->GetAddressSpaces(), [&] (const pmAddressSpace* pAddressSpace, size_t pAddressSpaceIndex)
        {
            if(pTask->IsWritable(pAddressSpace) && lSubscriptionManager.GetSubtaskShadowMem(this, pSubtaskId, pSplitInfo, (int)pAddressSpaceIndex))
            {
                if(pTask->IsReadWrite(pAddressSpace) && !pTask->HasDisjointReadWritesAcrossSubtasks(pAddressSpace))
                    lDeferCommit = true;
                else
                    pTask->GetSubscriptionManager().CommitSubtaskShadowMem(this, pSubtaskId, pSplitInfo, (int)pAddressSpaceIndex);
            }
        });
    }

    if(lDeferCommit)
        DeferShadowMemCommit(pTask, pSubtaskId, pSplitInfo);
}

void pmExecutionStub::DoSubtaskReduction(pmTask* pTask, ulong pSubtaskId1, pmSplitInfo* pSplitInfo1, pmExecutionStub* pStub2, ulong pSubtaskId2, pmSplitInfo* pSplitInfo2)
{
    TLS_IMPLEMENTATION_CLASS::GetTls()->SetThreadLocalStorage(TLS_CURRENT_SUBTASK_ID, &pSubtaskId1);
	pmStatus lStatus = pTask->GetCallbackUnit()->GetDataReductionCB()->Invoke(pTask, this, pSubtaskId1, pSplitInfo1, true, pStub2, pSubtaskId2, pSplitInfo2, true);
    TLS_IMPLEMENTATION_CLASS::GetTls()->SetThreadLocalStorage(TLS_CURRENT_SUBTASK_ID, NULL);

    const std::vector<pmAddressSpace*>& lAddressSpaceVector = pTask->GetAddressSpaces();

    std::vector<pmAddressSpace*>::const_iterator lIter = lAddressSpaceVector.begin(), lEndIter = lAddressSpaceVector.end();
    for(uint lMemIndex = 0; lIter != lEndIter; ++lIter, ++lMemIndex)
    {
        const pmAddressSpace* lAddressSpace = (*lIter);
        
        if(pTask->IsWritable(lAddressSpace))
        {
            pTask->GetSubscriptionManager().DestroySubtaskShadowMem(pStub2, pSubtaskId2, pSplitInfo2, lMemIndex);
            
            if(lStatus != pmSuccess)
                pTask->GetSubscriptionManager().DestroySubtaskShadowMem(this, pSubtaskId1, pSplitInfo1, lMemIndex);
        }
	}
    
    if(lStatus == pmSuccess)
        pTask->GetReducer()->AddSubtask(this, pSubtaskId1, pSplitInfo1);
}

void pmExecutionStub::WaitForNetworkFetch(const std::vector<pmCommandPtr>& pNetworkCommands)
{
    if(pNetworkCommands.empty())
        return;

#ifdef _DEBUG
    // Auto lock/unlock scope
    {
        FINALIZE_RESOURCE_PTR(dCurrentSubtaskLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCurrentSubtaskRangeLock, Lock(), Unlock());
        
        DEBUG_EXCEPTION_ASSERT(mCurrentSubtaskRangeStats);
    }
#endif
    
#ifdef ENABLE_TASK_PROFILING
    pmTask* lTask = NULL;

    // Auto lock/unlock scope
    {
        FINALIZE_RESOURCE_PTR(dCurrentSubtaskLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCurrentSubtaskRangeLock, Lock(), Unlock());

        lTask = mCurrentSubtaskRangeStats->task;
    }

    pmRecordProfileEventAutoPtr lRecordProfileEventAutoPtr(lTask->GetTaskProfiler(), taskProfiler::STUB_WAIT_ON_NETWORK);
#endif

    pmCommandPtr lAccumulatorCommand = pmAccumulatorCommand::CreateSharedPtr(pNetworkCommands);

    guarded_ptr<RESOURCE_LOCK_IMPLEMENTATION_CLASS, pmCommandPtr> lGuardedPtr(&mCurrentSubtaskRangeLock, &(mCurrentSubtaskRangeStats->accumulatorCommandPtr), &lAccumulatorCommand);

    if(RequiresPrematureExit())
        PMTHROW_NODUMP(pmPrematureExitException(false));

    pmStatus lStatus = lAccumulatorCommand->WaitForFinish();
    
    if(RequiresPrematureExit())
        PMTHROW_NODUMP(pmPrematureExitException(false));

    if(lStatus != pmSuccess)
        PMTHROW(pmMemoryFetchException());
}


/* struct currentSubtaskStats */
pmExecutionStub::currentSubtaskRangeStats::currentSubtaskRangeStats(pmTask* pTask, ulong pStartSubtaskId, ulong pEndSubtaskId, bool pOriginalAllottee, ulong pParentRangeStartSubtask, sigjmp_buf* pJmpBuf, double pStartTime, pmSplitInfo* pSplitInfo, pmExecutionStub* pSplitSubtaskSourceStub)
    : task(pTask)
    , startSubtaskId(pStartSubtaskId)
    , endSubtaskId(pEndSubtaskId)
    , currentSubtaskId(std::numeric_limits<ulong>::max())
    , parentRangeStartSubtask(pParentRangeStartSubtask)
    , currentSubtaskIdValid(false)
    , currentSubtaskInPostDataFetchStage(false)
    , originalAllottee(pOriginalAllottee)
    , startTime(pStartTime)
    , reassigned(false)
    , forceAckFlag(false)
    , prematureTermination(false)
    , taskListeningOnCancellation(false)
    , jmpBuf(pJmpBuf)
    , accumulatorCommandPtr(NULL)
#ifdef SUPPORT_SPLIT_SUBTASKS
    , splitSubtaskSourceStub(pSplitSubtaskSourceStub)
    , splitData(false, 0, 0)
#endif
{
#ifdef SUPPORT_SPLIT_SUBTASKS
    pmSplitData::ConvertSplitInfoToSplitData(splitData, pSplitInfo);
#endif
}

void pmExecutionStub::currentSubtaskRangeStats::ResetEndSubtaskId(ulong pEndSubtaskId)
{
    endSubtaskId = pEndSubtaskId;
}

/* class pmStubCPU */
pmStubCPU::pmStubCPU(size_t pCoreId, uint pDeviceIndexOnMachine)
	: pmExecutionStub(pDeviceIndexOnMachine)
{
	mCoreId = pCoreId;
}

pmStubCPU::~pmStubCPU()
{
}

void pmStubCPU::BindToProcessingElement()
{
	 SetProcessorAffinity((int)mCoreId);
}

size_t pmStubCPU::GetCoreId()
{
	return mCoreId;
}

std::string pmStubCPU::GetDeviceName()
{
	// Try to use processor_info or getcpuid system calls
	return std::string();
}

std::string pmStubCPU::GetDeviceDescription()
{
	// Try to use processor_info or getcpuid system calls
	return std::string();
}

pmDeviceType pmStubCPU::GetType() const
{
	return CPU;
}
    
ulong pmStubCPU::FindCollectivelyExecutableSubtaskRangeEnd(const pmSubtaskRange& pSubtaskRange, pmSplitInfo* pSplitInfo, bool pMultiAssign)
{
    return pSubtaskRange.startSubtask;
}

void pmStubCPU::Execute(pmTask* pTask, ulong pSubtaskId, bool pIsMultiAssign, ulong* pPreftechSubtaskIdPtr, pmSplitInfo* pSplitInfo /* = NULL */)
{
	CommonPreExecuteOnCPU(pTask, pSubtaskId, pIsMultiAssign, false, pSplitInfo);

    if(pPreftechSubtaskIdPtr)
        CommonPreExecuteOnCPU(pTask, *pPreftechSubtaskIdPtr, pIsMultiAssign, true, NULL);
    
    // Unless required for an address space, no shadow memory is created. In case user has asked for Compact subscription view, we need to create shadow mem
    pmSubscriptionManager& lSubscriptionManager = pTask->GetSubscriptionManager();
    for_each_with_index(pTask->GetAddressSpaces(), [&] (const pmAddressSpace* pAddressSpace, size_t pAddressSpaceIndex)
    {
        if(pTask->GetAddressSpaceSubscriptionVisibility(pAddressSpace, this) == SUBSCRIPTION_COMPACT)
        {
            if(!lSubscriptionManager.GetSubtaskShadowMem(this, pSubtaskId, pSplitInfo, (uint)pAddressSpaceIndex))
                lSubscriptionManager.CreateSubtaskShadowMem(this, pSubtaskId, pSplitInfo, (uint)pAddressSpaceIndex);
        }
    });
    
    const pmSubtaskInfo& lSubtaskInfo = pTask->GetSubscriptionManager().GetSubtaskInfo(this, pSubtaskId, pSplitInfo);
    
	INVOKE_SAFE_THROW_ON_FAILURE(pmSubtaskCB, pTask->GetCallbackUnit()->GetSubtaskCB(), Invoke, this, pTask, pSplitInfo, pIsMultiAssign, pTask->GetTaskInfo(), lSubtaskInfo);
}

void pmStubCPU::PrepareForSubtaskRangeExecution(pmTask* pTask, ulong pStartSubtaskId, ulong pEndSubtaskId, pmSplitInfo* pSplitInfo)
{
    DEBUG_EXCEPTION_ASSERT(pStartSubtaskId == pEndSubtaskId);
}
    
void pmStubCPU::WaitForSubtaskExecutionToFinish(pmTask* pTask, ulong pRangeStartSubtaskId, ulong pSubtaskId, pmSplitInfo* pSplitInfo)
{
}
    
void pmStubCPU::CleanupPostSubtaskRangeExecution(pmTask* pTask, ulong pStartSubtaskId, ulong pEndSubtaskId, ulong pCleanupEndSubtaskId, pmSplitInfo* pSplitInfo)
{
}
    
// This method must be called with mCurrentSubtaskRangeLock (of pmExecutionStub) acquired
void pmStubCPU::TerminateUserModeExecution()
{
    InterruptThread();
}


/* class pmStubGPU */
pmStubGPU::pmStubGPU(uint pDeviceIndexOnMachine)
	: pmExecutionStub(pDeviceIndexOnMachine)
{
}

pmStubGPU::~pmStubGPU()
{
}

#ifdef SUPPORT_CUDA
/* class pmStubCUDA */
pmStubCUDA::pmStubCUDA(size_t pDeviceIndex, uint pDeviceIndexOnMachine)
	: pmStubGPU(pDeviceIndexOnMachine)
    , mDeviceIndex(pDeviceIndex)
    , mDeviceInfoCudaPtr(NULL)
    , mCudaCache(pmCudaCacheEvictor(this))
    , mStartSubtaskId(std::numeric_limits<ulong>::max())
#ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
#else
    , mStatusCopySrc(pmStatusUnavailable)
    , mStatusCopyDest(pmStatusUnavailable)
#endif
{
}

void pmStubCUDA::FreeResources()
{
    FreeGpuResources();
}

void pmStubCUDA::FreeExecutionResources()
{
#ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
    mPinnedChunkCollection.Reset();
#endif

    if(mDeviceInfoCudaPtr)
        DestroyDeviceInfoCudaPtr(mDeviceInfoCudaPtr);

    mCudaChunkCollection.Reset();
    mScratchChunkCollection.Reset();

    EXCEPTION_ASSERT(mTaskInfoCudaMap.empty());
    
    pmCudaInterface::UnbindFromDevice(mDeviceIndex);
}
    
size_t pmStubCUDA::GetDeviceIndex()
{
    return mDeviceIndex;
}

pmAllocatorCollection<pmCudaMemChunkTraits>* pmStubCUDA::GetCudaChunkCollection()
{
    return &mCudaChunkCollection;
}

void pmStubCUDA::FreeTaskResources(const pmMachine* pOriginatingHost, ulong pSequenceNumber)
{
    std::pair<const pmMachine*, ulong> lPair(pOriginatingHost, pSequenceNumber);

    std::map<std::pair<const pmMachine*, ulong>, pmTaskInfo>::iterator lIter = mTaskInfoCudaMap.find(lPair);
    if(lIter != mTaskInfoCudaMap.end())
    {
        if(lIter->second.taskConfLength)
            DestroyTaskConf(lIter->second.taskConf);
    
        mTaskInfoCudaMap.erase(lPair);
    }
}

void pmStubCUDA::BindToProcessingElement()
{
	pmCudaInterface::BindToDevice(mDeviceIndex);
}

std::string pmStubCUDA::GetDeviceName()
{
	return pmCudaInterface::GetDeviceName(mDeviceIndex);
}

std::string pmStubCUDA::GetDeviceDescription()
{
	return pmCudaInterface::GetDeviceDescription(mDeviceIndex);
}

pmDeviceType pmStubCUDA::GetType() const
{
	return GPU_CUDA;
}
    
void pmStubCUDA::PurgeAddressSpaceEntriesFromGpuCache(const pmAddressSpace* pAddressSpace)
{
    mCudaCache.RemoveKeys([pAddressSpace] (const pmCudaCacheKey& pKey) -> bool
                          {
                              return (pAddressSpace == pKey.addressSpace);
                          });
}
    
std::unique_ptr<pmCudaCacheKey> pmStubCUDA::MakeCudaCacheKey(pmTask* pTask, ulong pSubtaskId, pmSplitInfo* pSplitInfo, uint pAddressSpaceIndex, const pmAddressSpace* pAddressSpace, pmSubscriptionVisibilityType pVisibilityType)
{
    DEBUG_EXCEPTION_ASSERT(pTask->IsCudaCacheEnabled());

    std::unique_ptr<pmCudaCacheKey> lCacheKeyPtr;
    
    pmSubscriptionManager& lSubscriptionManager = pTask->GetSubscriptionManager();
    pmSubscriptionFormat lFormat = lSubscriptionManager.GetSubscriptionFormat(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex);
    
    switch(lFormat)
    {
        case SUBSCRIPTION_CONTIGUOUS:
            if(pVisibilityType == SUBSCRIPTION_NATURAL)
                lCacheKeyPtr.reset(new pmCudaCacheKey(pAddressSpace, pVisibilityType, lSubscriptionManager.GetUnifiedReadWriteSubscription(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex)));
            else    // SUBSCRIPTION_COMPACT
                lCacheKeyPtr.reset(new pmCudaCacheKey(pAddressSpace, pVisibilityType, lSubscriptionManager.GetCompactedSubscription(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex).subscriptionInfo));
            
            break;
            
        case SUBSCRIPTION_SCATTERED:
            lCacheKeyPtr.reset(new pmCudaCacheKey(pAddressSpace, pVisibilityType, lSubscriptionManager.GetUnifiedScatteredSubscriptionInfoVector(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex)));
            break;
            
        case SUBSCRIPTION_GENERAL:
            lCacheKeyPtr.reset(new pmCudaCacheKey(pAddressSpace, pVisibilityType, lSubscriptionManager.GetNonConsolidatedReadWriteSubscriptions(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex)));
            break;
            
        default:
            PMTHROW(pmFatalErrorException());
    }
    
    return lCacheKeyPtr;
}
    
void* pmStubCUDA::AllocateMemoryOnDevice(size_t pLength, size_t pCudaAlignment, pmAllocatorCollection<pmCudaMemChunkTraits>& pChunkCollection)
{
    void* lPtr = pChunkCollection.AllocateNoThrow(pLength, pCudaAlignment);

    while(!lPtr && mCudaCache.Purge())
        lPtr = pChunkCollection.AllocateNoThrow(pLength, pCudaAlignment);
    
    return lPtr;
}

bool pmStubCUDA::AllocateMemoryForDeviceCopy(size_t pLength, size_t pCudaAlignment, pmCudaSubtaskMemoryStruct& pMemoryStruct, pmAllocatorCollection<pmCudaMemChunkTraits>& pChunkCollection)
{
    pMemoryStruct.cudaPtr = AllocateMemoryOnDevice(pLength, pCudaAlignment, pChunkCollection);

#ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
    if(pMemoryStruct.cudaPtr)
    {
        pMemoryStruct.pinnedPtr = mPinnedChunkCollection.AllocateNoThrow(pLength, pCudaAlignment);
        
        if(!pMemoryStruct.pinnedPtr)
        {
            pChunkCollection.Deallocate(pMemoryStruct.cudaPtr);
            pMemoryStruct.cudaPtr = NULL;
        }
    }
#endif
    
    return (pMemoryStruct.cudaPtr != NULL);
}
    
bool pmStubCUDA::CheckSubtaskMemoryRequirements(pmTask* pTask, ulong pSubtaskId, pmSplitInfo* pSplitInfo, std::vector<std::shared_ptr<pmCudaCacheValue>>& pPreventCachePurgeVector, size_t pCudaAlignment)
{
    bool lLoadStatus = true;    // Can this subtask's memory requirements be loaded (or are already loaded) on the device?

    pmSubscriptionManager& lSubscriptionManager = pTask->GetSubscriptionManager();
    lSubscriptionManager.FindSubtaskMemDependencies(this, pSubtaskId, pSplitInfo);
    
    const size_t lAddressSpaceCount = pTask->GetAddressSpaceCount();
    const bool lCudaCacheEnabledForTask = pTask->IsCudaCacheEnabled();
    
    std::vector<pmCudaSubtaskMemoryStruct> lSubtaskMemoryVector(lAddressSpaceCount);
    std::vector<std::pair<std::unique_ptr<pmCudaCacheKey>, std::shared_ptr<pmCudaCacheValue>>> lPendingCacheInsertions;

    std::unique_ptr<pmCudaCacheKey> lCudaCacheKeyPtr;

    // Data is not fetched till this stage and shadow mem is also not created; so not using GetSubtaskInfo call here
    for_each_with_index(pTask->GetAddressSpaces(), [&] (const pmAddressSpace* pAddressSpace, size_t pAddressSpaceIndex)
    {
        bool lNeedsAllocation = true;

        pmSubscriptionVisibilityType lVisibilityType = pTask->GetAddressSpaceSubscriptionVisibility(pAddressSpace, this);
        
        if(lCudaCacheEnabledForTask)
            lCudaCacheKeyPtr = MakeCudaCacheKey(pTask, pSubtaskId, pSplitInfo, (uint)pAddressSpaceIndex, pAddressSpace, lVisibilityType);

        pmSubscriptionInfo lSubscriptionInfo;
        
        /* If address space is read-only, we need to ensure that the GPU copy of data (if any) is latest - need to cross check memory directory - partially done yet (PurgeAddressSpaceEntriesFromGpuCache)
         * If address space is write-only without reduction, we need to reuse the GPU copy of data (if any)
         * If address space is write-only with reduction, then GPU copy of data can not be reused and new data can not be cached
         * If address space is read-write with disjoint reads and writes across subtasks, this can be treated as read-only case
         * if address space is read-write without disjoint reads and writes across subtasks, this should be treated as write-only with reduction case.
        */

        if(lVisibilityType == SUBSCRIPTION_NATURAL)
        {
            if(pTask->IsReadOnly(pAddressSpace))
                lSubscriptionInfo = lSubscriptionManager.GetConsolidatedReadSubscription(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex);
            else
                lSubscriptionInfo = lSubscriptionManager.GetUnifiedReadWriteSubscription(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex);
        }
        else    // SUBSCRIPTION_COMPACT
        {
            lSubscriptionInfo = lSubscriptionManager.GetCompactedSubscription(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex).subscriptionInfo;
        }
        
        bool lTemporaryAddressSpaceData = ((pTask->IsWriteOnly(pAddressSpace) && pTask->GetCallbackUnit()->GetDataReductionCB()) || (pTask->IsReadWrite(pAddressSpace) && !pTask->HasDisjointReadWritesAcrossSubtasks(pAddressSpace)));
        
        if(lSubscriptionInfo.length && lCudaCacheEnabledForTask && !lTemporaryAddressSpaceData)
        {
            std::shared_ptr<pmCudaCacheValue>& lDeviceMemoryPtr = mCudaCache.Get(*lCudaCacheKeyPtr.get());
            
            if(lDeviceMemoryPtr.get())
            {
                // For writeable address spaces, we can reuse existing CUDA memory but pinned mem is required to copy data out
                if(pTask->IsWritable(pAddressSpace))
                {
                    lSubtaskMemoryVector[pAddressSpaceIndex].requiresLoad = true;
                    lSubtaskMemoryVector[pAddressSpaceIndex].pinnedPtr = mPinnedChunkCollection.AllocateNoThrow(lSubscriptionInfo.length, pCudaAlignment);
                    
                    if(!lSubtaskMemoryVector[pAddressSpaceIndex].pinnedPtr)
                    {
                        lLoadStatus = false;
                        return;     // return from lambda expression
                    }
                }

                lSubtaskMemoryVector[pAddressSpaceIndex].cudaPtr = lDeviceMemoryPtr->cudaPtr;
                pPreventCachePurgeVector.push_back(lDeviceMemoryPtr);   // increase ref count of cache value
                lNeedsAllocation = false;
            }
        }

        if(lNeedsAllocation && lSubscriptionInfo.length)
        {
            if(!AllocateMemoryForDeviceCopy(lSubscriptionInfo.length, pCudaAlignment, lSubtaskMemoryVector[pAddressSpaceIndex], mCudaChunkCollection))
            {
                lLoadStatus = false;
                return;     // return from lambda expression
            }

            lSubtaskMemoryVector[pAddressSpaceIndex].requiresLoad = true;
            
            if(lCudaCacheEnabledForTask)
            {
                if(lTemporaryAddressSpaceData)
                    lSubtaskMemoryVector[pAddressSpaceIndex].isUncached = true;
                else
                    lPendingCacheInsertions.emplace_back(std::move(lCudaCacheKeyPtr), std::shared_ptr<pmCudaCacheValue>(new pmCudaCacheValue(lSubtaskMemoryVector[pAddressSpaceIndex].cudaPtr)));
            }
        }
    });

    if(lLoadStatus)
    {
        pmScratchBufferType lScratchBufferType = SUBTASK_TO_POST_SUBTASK;
        size_t lScratchBufferSize = 0;
        void* lCpuScratchBuffer = lSubscriptionManager.CheckAndGetScratchBuffer(this, pSubtaskId, pSplitInfo, lScratchBufferSize, lScratchBufferType);

        if(lCpuScratchBuffer && lScratchBufferSize)
        {
            pmCudaSubtaskMemoryStruct lStruct;
            lLoadStatus = false;

            if(AllocateMemoryForDeviceCopy(lScratchBufferSize, pCudaAlignment, lStruct, mScratchChunkCollection))
            {
                lStruct.requiresLoad = true;
                lLoadStatus = true;

                lSubtaskMemoryVector.push_back(lStruct);
            }
        }
    }

    if(lLoadStatus)
    {
        pmCudaSubtaskSecondaryBuffersStruct& lStruct = mSubtaskSecondaryBuffersMap.emplace(std::make_pair(pSubtaskId, pmCudaSubtaskSecondaryBuffersStruct())).first->second;

    #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
        lStruct.statusPinnedPtr = mPinnedChunkCollection.AllocateNoThrow(sizeof(pmStatus), pCudaAlignment);
        
        if(!lStruct.statusPinnedPtr)
        {
            lLoadStatus = false;
        }
        else
    #endif
        {
            size_t lReservedMem = lSubscriptionManager.GetReservedCudaGlobalMemSize(this, pSubtaskId, pSplitInfo);
            size_t lTotalMem = lReservedMem + sizeof(pmStatus);

            if(lTotalMem)
            {
                void* lPtr = NULL;
                lLoadStatus = false;

                if((lPtr = AllocateMemoryOnDevice(lTotalMem, pCudaAlignment, mScratchChunkCollection)) != NULL)
                {
                    lLoadStatus = true;

                    if(lReservedMem)
                    {
                        lStruct.reservedMemCudaPtr = lPtr;
                        lStruct.statusCudaPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(lStruct.reservedMemCudaPtr) + lReservedMem);
                    }
                    else
                    {
                        lStruct.statusCudaPtr = lPtr;
                    }
                }
            #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
                else
                {
                    mPinnedChunkCollection.Deallocate(lStruct.statusPinnedPtr);
                }
            #endif
            }
        }
    }

    if(lLoadStatus)
    {
        std::move(lSubtaskMemoryVector.begin(), lSubtaskMemoryVector.end(), std::back_inserter(mSubtaskPointersMap[pSubtaskId]));
            
        if(lCudaCacheEnabledForTask)
        {
            for_each(lPendingCacheInsertions, [&] (decltype(lPendingCacheInsertions)::value_type& pPair)
            {
                mCacheKeys[pSubtaskId].push_back(*pPair.first.get());
                mCudaCache.Insert(*pPair.first.get(), pPair.second);
                pPreventCachePurgeVector.push_back(pPair.second);   // increase ref count to prevent purging
            });
        }
    }
    else
    {
        mSubtaskSecondaryBuffersMap.erase(pSubtaskId);

        for_each_with_index(lSubtaskMemoryVector, [&] (const pmCudaSubtaskMemoryStruct& pStruct, size_t pIndex)
        {
            if(pStruct.requiresLoad)
            {
                if(pIndex == lAddressSpaceCount)
                    mScratchChunkCollection.Deallocate(pStruct.cudaPtr);
                else
                    mCudaChunkCollection.Deallocate(pStruct.cudaPtr);

            #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
                mPinnedChunkCollection.Deallocate(pStruct.pinnedPtr);
            #endif
            }
        });
    }

    return lLoadStatus;
}
    
bool pmStubCUDA::InitializeCudaStream(std::shared_ptr<pmCudaStreamAutoPtr>& pSharedPtr)
{
    while(1)
    {
        try
        {
            pSharedPtr->Initialize(pmDispatcherGPU::GetDispatcherGPU()->GetDispatcherCUDA()->GetRuntimeHandle());
            return true;
        }
        catch(pmOutOfMemoryException&)
        {
            if(!mCudaCache.Purge())
                return false;
        }
    }
    
    return true;
}

ulong pmStubCUDA::FindCollectivelyExecutableSubtaskRangeEnd(const pmSubtaskRange& pSubtaskRange, pmSplitInfo* pSplitInfo, bool pMultiAssign)
{
    DEBUG_EXCEPTION_ASSERT(!pSplitInfo || (pSubtaskRange.startSubtask == pSubtaskRange.endSubtask && !pMultiAssign));
    
    pmSubtaskRange lSubtaskRange(pSubtaskRange);

#ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
    if(!pSubtaskRange.task->ShouldOverlapComputeCommunication())
        lSubtaskRange.endSubtask = lSubtaskRange.startSubtask;
#else
    lSubtaskRange.endSubtask = lSubtaskRange.startSubtask;
#endif

    ulong lSubtaskCount = 0;
    pmTask* lTask = lSubtaskRange.task;

    std::vector<std::shared_ptr<pmCudaCacheValue>> lPreventCachePurgeVector;
    size_t lCudaAlignment = pmCudaInterface::GetCudaAlignment(mDeviceIndex);

    for(ulong lSubtaskId = lSubtaskRange.startSubtask; lSubtaskId <= lSubtaskRange.endSubtask; ++lSubtaskId, ++lSubtaskCount)
    {
        std::shared_ptr<pmCudaStreamAutoPtr> lSharedPtr(new pmCudaStreamAutoPtr());
        
        if(!InitializeCudaStream(lSharedPtr))
            break;

        if(!CheckSubtaskMemoryRequirements(lTask, lSubtaskId, pSplitInfo, lPreventCachePurgeVector, lCudaAlignment))
            break;
        
        mCudaStreams.push_back(std::move(lSharedPtr));
    }
    
    EXCEPTION_ASSERT(lSubtaskCount);    // not enough CUDA memory to run even one subtask

    return (lSubtaskRange.startSubtask + lSubtaskCount - 1);
}

void pmStubCUDA::Execute(pmTask* pTask, ulong pSubtaskId, bool pIsMultiAssign, ulong* pPreftechSubtaskIdPtr, pmSplitInfo* pSplitInfo /* = NULL */)
{
	CommonPreExecuteOnCPU(pTask, pSubtaskId, pIsMultiAssign, false, pSplitInfo);

    if(pPreftechSubtaskIdPtr)
        CommonPreExecuteOnCPU(pTask, *pPreftechSubtaskIdPtr, pIsMultiAssign, true, NULL);

    // Unless required for an address space, no shadow memory is created. In case user has asked for Compact subscription view and task has redistribution, we need to create shadow mem
    if(pTask->GetCallbackUnit()->GetDataRedistributionCB())
    {
        pmSubscriptionManager& lSubscriptionManager = pTask->GetSubscriptionManager();
        for_each_with_index(pTask->GetAddressSpaces(), [&] (const pmAddressSpace* pAddressSpace, size_t pAddressSpaceIndex)
        {
            if(pTask->GetAddressSpaceSubscriptionVisibility(pAddressSpace, this) == SUBSCRIPTION_COMPACT)
            {
                if(!lSubscriptionManager.GetSubtaskShadowMem(this, pSubtaskId, pSplitInfo, (uint)pAddressSpaceIndex))
                    lSubscriptionManager.CreateSubtaskShadowMem(this, pSubtaskId, pSplitInfo, (uint)pAddressSpaceIndex);
            }
        });
    }

    const pmSubtaskInfo& lSubtaskInfo = pTask->GetSubscriptionManager().GetSubtaskInfo(this, pSubtaskId, pSplitInfo);

#ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
    CopyDataToPinnedBuffers(pTask, pSubtaskId, pSplitInfo, lSubtaskInfo);
#endif

    PopulateMemcpyCommands(pTask, pSubtaskId, pSplitInfo, lSubtaskInfo);

    std::pair<const pmMachine*, ulong> lPair(pTask->GetOriginatingHost(), pTask->GetSequenceNumber());

    std::map<std::pair<const pmMachine*, ulong>, pmTaskInfo>::iterator lIter = mTaskInfoCudaMap.find(lPair);
    if(lIter == mTaskInfoCudaMap.end())
    {
        lIter = mTaskInfoCudaMap.insert(std::make_pair(lPair, pTask->GetTaskInfo())).first;
        lIter->second.taskConf = CreateTaskConf(pTask->GetTaskInfo());
    }

	INVOKE_SAFE_THROW_ON_FAILURE(pmSubtaskCB, pTask->GetCallbackUnit()->GetSubtaskCB(), Invoke, this, pTask, pSplitInfo, pIsMultiAssign, lIter->second, lSubtaskInfo, mCudaStreams[pSubtaskId - mStartSubtaskId].get());
}

void* pmStubCUDA::CreateTaskConf(const pmTaskInfo& pTaskInfo)
{
    void* lTaskConfCudaPtr = NULL;

    if(pTaskInfo.taskConfLength)
    {
        size_t lCudaAlignment = pmCudaInterface::GetCudaAlignment(mDeviceIndex);

        lTaskConfCudaPtr = mScratchChunkCollection.AllocateNoThrow((size_t)pTaskInfo.taskConfLength, lCudaAlignment);
        if(!lTaskConfCudaPtr)
            PMTHROW(pmOutOfMemoryException());

        pmCudaInterface::CopyDataToCudaDevice(lTaskConfCudaPtr, pTaskInfo.taskConf, pTaskInfo.taskConfLength);
    }
    
    return lTaskConfCudaPtr;
}

void pmStubCUDA::DestroyTaskConf(void* pTaskConfCudaPtr)
{
    mScratchChunkCollection.Deallocate(pTaskConfCudaPtr);
}

void* pmStubCUDA::CreateDeviceInfoCudaPtr(const pmDeviceInfo& pDeviceInfo)
{
    size_t lCudaAlignment = pmCudaInterface::GetCudaAlignment(mDeviceIndex);

    void* lDeviceInfoCudaPtr = mScratchChunkCollection.AllocateNoThrow(sizeof(pmDeviceInfo), lCudaAlignment);
    if(!lDeviceInfoCudaPtr)
        PMTHROW(pmOutOfMemoryException());

    pmCudaInterface::CopyDataToCudaDevice(lDeviceInfoCudaPtr, &pDeviceInfo, sizeof(pmDeviceInfo));
    
    return lDeviceInfoCudaPtr;
}

void pmStubCUDA::DestroyDeviceInfoCudaPtr(void* pDeviceInfoCudaPtr)
{
    mScratchChunkCollection.Deallocate(pDeviceInfoCudaPtr);
}
    
void* pmStubCUDA::GetDeviceInfoCudaPtr()
{
    if(!mDeviceInfoCudaPtr)
        mDeviceInfoCudaPtr = CreateDeviceInfoCudaPtr(GetProcessingElement()->GetDeviceInfo());
        
    return mDeviceInfoCudaPtr;
}

void pmStubCUDA::PopulateMemcpyCommands(pmTask* pTask, ulong pSubtaskId, pmSplitInfo* pSplitInfo, const pmSubtaskInfo& pSubtaskInfo)
{
    mDeviceToHostCommands.clear();
    mHostToDeviceCommands.clear();

    pmSubscriptionManager& lSubscriptionManager = pTask->GetSubscriptionManager();

    std::vector<pmCudaSubtaskMemoryStruct>& lVector = mSubtaskPointersMap[pSubtaskId];

    uint lAddressSpaceCount = pTask->GetAddressSpaceCount();

    DEBUG_EXCEPTION_ASSERT(lVector.size() <= lAddressSpaceCount + 1);
    
    for_each_with_index(pTask->GetAddressSpaces(), [&] (const pmAddressSpace* pAddressSpace, size_t pAddressSpaceIndex)
    {
        if(lVector[pAddressSpaceIndex].requiresLoad)
        {
            pmSubscriptionVisibilityType lVisibilityType = pTask->GetAddressSpaceSubscriptionVisibility(pAddressSpace, this);

            subscription::subscriptionRecordType::const_iterator lBegin, lEnd, lIter;
            lSubscriptionManager.GetNonConsolidatedReadSubscriptions(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex, lBegin, lEnd);

            if(lVisibilityType == SUBSCRIPTION_NATURAL)
            {
                pmSubscriptionInfo lSubscriptionInfo;

                if(pTask->IsReadOnly(pAddressSpace))
                    lSubscriptionInfo = lSubscriptionManager.GetConsolidatedReadSubscription(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex);
                else
                    lSubscriptionInfo = lSubscriptionManager.GetUnifiedReadWriteSubscription(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex);

                if(lSubscriptionInfo.length)
                {
                    for(lIter = lBegin; lIter != lEnd; ++lIter)
                    {
                    #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
                        void* lSrcPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(lVector[pAddressSpaceIndex].pinnedPtr) + lIter->first - lSubscriptionInfo.offset);
                    #else
                        void* lSrcPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(pSubtaskInfo.memInfo[pAddressSpaceIndex].ptr) + lIter->first - lSubscriptionInfo.offset);
                    #endif
                        
                        void* lDestPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(lVector[pAddressSpaceIndex].cudaPtr) + lIter->first - lSubscriptionInfo.offset);

                        mHostToDeviceCommands.push_back(pmCudaMemcpyCommand(lSrcPtr, lDestPtr, lIter->second.first));
                    }
                }
            }
            else    // SUBSCRIPTION_COMPACT
            {
                const subscription::pmCompactViewData& lCompactViewData = lSubscriptionManager.GetCompactedSubscription(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex);

                if(lCompactViewData.subscriptionInfo.length)
                {
                    auto lOffsetsIter = lCompactViewData.nonConsolidatedReadSubscriptionOffsets.begin();
                    DEBUG_EXCEPTION_ASSERT(std::distance(lOffsetsIter, lCompactViewData.nonConsolidatedReadSubscriptionOffsets.end()) == std::distance(lBegin, lEnd));

                #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP    // src data comes from pinned buffer
                    if(pTask->IsReadOnly(pAddressSpace))
                    {
                        mHostToDeviceCommands.push_back(pmCudaMemcpyCommand(lVector[pAddressSpaceIndex].pinnedPtr, lVector[pAddressSpaceIndex].cudaPtr, lCompactViewData.subscriptionInfo.length));
                    }
                    else
                    {
                        for(lIter = lBegin; lIter != lEnd; ++lIter, ++lOffsetsIter)
                        {
                            void* lSrcPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(lVector[pAddressSpaceIndex].pinnedPtr) + (*lOffsetsIter));
                            void* lDestPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(lVector[pAddressSpaceIndex].cudaPtr) + (*lOffsetsIter));

                            mHostToDeviceCommands.push_back(pmCudaMemcpyCommand(lSrcPtr, lDestPtr, lIter->second.first));
                        }
                    }
                #else   // src data comes from shadow memory (if any) or task mem
                    void* lShadowMemAddr = pSubtaskInfo.memInfo[pAddressSpaceIndex].ptr;
                    
                    if(lShadowMemAddr && pTask->IsReadOnly(pAddressSpace))
                    {
                        mHostToDeviceCommands.push_back(pmCudaMemcpyCommand(lShadowMemAddr, lVector[pAddressSpaceIndex].cudaPtr, lCompactViewData.subscriptionInfo.length));
                    }
                    else
                    {
                        size_t lBaseAddr = reinterpret_cast<size_t>(pAddressSpace->GetMem());

                        for(lIter = lBegin; lIter != lEnd; ++lIter, ++lOffsetsIter)
                        {
                            void* lSrcPtr = reinterpret_cast<void*>((lShadowMemAddr ? (lShadowMemAddr + (*lOffsetsIter)) : (lBaseAddr + lIter->first)));
                            void* lDestPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(lVector[pAddressSpaceIndex].cudaPtr) + (*lOffsetsIter));

                            mHostToDeviceCommands.push_back(pmCudaMemcpyCommand(lSrcPtr, lDestPtr, lIter->second.first));
                        }
                    }
                #endif
                }
            }
        }
    });
    
    if(lVector.size() > lAddressSpaceCount)
    {
        pmScratchBufferType lScratchBufferType = SUBTASK_TO_POST_SUBTASK;
        size_t lScratchBufferSize = 0;
        void* lCpuScratchBuffer = lSubscriptionManager.CheckAndGetScratchBuffer(this, pSubtaskId, pSplitInfo, lScratchBufferSize, lScratchBufferType);
        if(lCpuScratchBuffer && lScratchBufferSize)
        {
            if(lScratchBufferType == PRE_SUBTASK_TO_SUBTASK || lScratchBufferType == PRE_SUBTASK_TO_POST_SUBTASK)
            {
                pmCudaSubtaskMemoryStruct& lStruct = lVector.back();
                
            #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
                mHostToDeviceCommands.push_back(pmCudaMemcpyCommand(lStruct.pinnedPtr, lStruct.cudaPtr, lScratchBufferSize));
            #else
                mHostToDeviceCommands.push_back(pmCudaMemcpyCommand(lCpuScratchBuffer, lStruct.cudaPtr, lScratchBufferSize));
            #endif
            }
        }
    }

#if 0   // Not copying in status
    pmCudaSubtaskSecondaryBuffersStruct& lSecondaryStruct = mSubtaskSecondaryBuffersMap[pSubtaskId];
    if(lSecondaryStruct.statusCudaPtr)
    {
    #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
        mHostToDeviceCommands.push_back(pmCudaMemcpyCommand(lSecondaryStruct.statusPinnedPtr, lSecondaryStruct.statusCudaPtr, sizeof(pmStatus)));
    #else
        mHostToDeviceCommands.push_back(pmCudaMemcpyCommand((void*)(&mStatusCopySrc), lSecondaryStruct.statusCudaPtr, sizeof(pmStatus)));
    #endif
    }
#endif
    
    for_each_with_index(pTask->GetAddressSpaces(), [&] (const pmAddressSpace* pAddressSpace, size_t pAddressSpaceIndex)
    {
        if(!pTask->IsReadOnly(pAddressSpace))
        {
            pmSubscriptionVisibilityType lVisibilityType = pTask->GetAddressSpaceSubscriptionVisibility(pAddressSpace, this);

            subscription::subscriptionRecordType::const_iterator lBegin, lEnd, lIter;
            lSubscriptionManager.GetNonConsolidatedWriteSubscriptions(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex, lBegin, lEnd);

            if(lVisibilityType == SUBSCRIPTION_NATURAL)
            {
                pmSubscriptionInfo lSubscriptionInfo = lSubscriptionManager.GetUnifiedReadWriteSubscription(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex);

                if(lSubscriptionInfo.length)
                {
                    DEBUG_EXCEPTION_ASSERT(lVector[pAddressSpaceIndex].requiresLoad);
                    
                    for(lIter = lBegin; lIter != lEnd; ++lIter)
                    {
                    #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
                        void* lDestPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(lVector[pAddressSpaceIndex].pinnedPtr) + lIter->first - lSubscriptionInfo.offset);
                    #else
                        void* lDestPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(pSubtaskInfo.memInfo[pAddressSpaceIndex].ptr) + lIter->first - lSubscriptionInfo.offset);
                    #endif
                        
                        void* lSrcPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(lVector[pAddressSpaceIndex].cudaPtr) + lIter->first - lSubscriptionInfo.offset);

                        mDeviceToHostCommands.push_back(pmCudaMemcpyCommand(lSrcPtr, lDestPtr, lIter->second.first));
                    }
                }
            }
            else    // SUBSCRIPTION_COMPACT
            {
                const subscription::pmCompactViewData& lCompactViewData = lSubscriptionManager.GetCompactedSubscription(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex);

                if(lCompactViewData.subscriptionInfo.length)
                {
                    size_t lBaseAddr = reinterpret_cast<size_t>(pSubtaskInfo.memInfo[pAddressSpaceIndex].ptr);
                    if(pSubtaskInfo.memInfo[pAddressSpaceIndex].ptr)    // Non-null only if there is an associated shadow mem
                        lBaseAddr -= lCompactViewData.subscriptionInfo.offset;
                    else
                        lBaseAddr = reinterpret_cast<size_t>(pAddressSpace->GetMem());

                    auto lOffsetsIter = lCompactViewData.nonConsolidatedWriteSubscriptionOffsets.begin();
                    DEBUG_EXCEPTION_ASSERT(std::distance(lOffsetsIter, lCompactViewData.nonConsolidatedWriteSubscriptionOffsets.end()) == std::distance(lBegin, lEnd));

                #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP    // dest data comes from pinned buffer
                    if(pTask->IsWriteOnly(pAddressSpace))
                    {
                        mDeviceToHostCommands.push_back(pmCudaMemcpyCommand(lVector[pAddressSpaceIndex].cudaPtr, lVector[pAddressSpaceIndex].pinnedPtr, lCompactViewData.subscriptionInfo.length));
                    }
                    else
                    {
                        for(lIter = lBegin; lIter != lEnd; ++lIter, ++lOffsetsIter)
                        {
                            void* lDestPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(lVector[pAddressSpaceIndex].pinnedPtr) + (*lOffsetsIter));
                            void* lSrcPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(lVector[pAddressSpaceIndex].cudaPtr) + (*lOffsetsIter));

                            mDeviceToHostCommands.push_back(pmCudaMemcpyCommand(lSrcPtr, lDestPtr, lIter->second.first));
                        }
                    }
                #else   // dest data goes to shadow memory (if any) or task mem
                    void* lShadowMemAddr = pSubtaskInfo.memInfo[pAddressSpaceIndex].ptr;
                    
                    if(lShadowMemAddr && pTask->IsWriteOnly(pAddressSpace))
                    {
                        mDeviceToHostCommands.push_back(pmCudaMemcpyCommand(lVector[pAddressSpaceIndex].cudaPtr, lShadowMemAddr, lCompactViewData.subscriptionInfo.length));
                    }
                    else
                    {
                        size_t lBaseAddr = reinterpret_cast<size_t>(pAddressSpace->GetMem());

                        for(lIter = lBegin; lIter != lEnd; ++lIter, ++lOffsetsIter)
                        {
                            void* lDestPtr = reinterpret_cast<void*>((lShadowMemAddr ? (lShadowMemAddr + (*lOffsetsIter)) : (lBaseAddr + lIter->first)));
                            void* lSrcPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(lVector[pAddressSpaceIndex].cudaPtr) + (*lOffsetsIter));

                            mDeviceToHostCommands.push_back(pmCudaMemcpyCommand(lSrcPtr, lDestPtr, lIter->second.first));
                        }
                    }
                #endif
                }
            }
        }
    });

    if(lVector.size() > lAddressSpaceCount)
    {
        pmScratchBufferType lScratchBufferType = SUBTASK_TO_POST_SUBTASK;
        size_t lScratchBufferSize = 0;
        void* lCpuScratchBuffer = lSubscriptionManager.CheckAndGetScratchBuffer(this, pSubtaskId, pSplitInfo, lScratchBufferSize, lScratchBufferType);
        if(lCpuScratchBuffer && lScratchBufferSize)
        {
            if(lScratchBufferType == SUBTASK_TO_POST_SUBTASK || lScratchBufferType == PRE_SUBTASK_TO_POST_SUBTASK)
            {
                pmCudaSubtaskMemoryStruct& lStruct = lVector.back();

            #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
                mDeviceToHostCommands.push_back(pmCudaMemcpyCommand(lStruct.cudaPtr, lStruct.pinnedPtr, lScratchBufferSize));
            #else
                mDeviceToHostCommands.push_back(pmCudaMemcpyCommand(lStruct.cudaPtr, lCpuScratchBuffer, lScratchBufferSize));
            #endif
            }
        }
    }

#if 0   // Not reading out status
    if(lSecondaryStruct.statusCudaPtr)
    {
    #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
        mDeviceToHostCommands.push_back(pmCudaMemcpyCommand(lSecondaryStruct.statusCudaPtr, lSecondaryStruct.statusPinnedPtr, sizeof(pmStatus)));
    #else
        mDeviceToHostCommands.push_back(pmCudaMemcpyCommand(lSecondaryStruct.statusCudaPtr, (void*)(&mStatusCopyDest), sizeof(pmStatus)));
    #endif
    }
#endif
}

void pmStubCUDA::ReserveMemory(size_t pPhysicalMemory, size_t pTotalStubCount)
{
    size_t lPageSize = MEMORY_MANAGER_IMPLEMENTATION_CLASS::GetMemoryManager()->GetVirtualMemoryPageSize();
    size_t lMaxMem = std::min((size_t)4 * 1024 * 1024 * 1024, pmCudaInterface::GetAvailableCudaMem());

    if(pPhysicalMemory && pTotalStubCount)
        lMaxMem = std::min(lMaxMem, pPhysicalMemory / pTotalStubCount);
    
    size_t lGBs = std::max((size_t)1, lMaxMem / (1024 * 1024 * 1024));
    
    size_t lCudaChunkSizeMultiplier = CUDA_CHUNK_SIZE_MULTIPLIER_PER_GB * lGBs;
    size_t lScratchChunkSizeMultiplier = SCRATCH_CHUNK_SIZE_MULTIPLIER_PER_GB * lGBs;
    
    lCudaChunkSizeMultiplier = ((lCudaChunkSizeMultiplier / lPageSize) + ((lCudaChunkSizeMultiplier % lPageSize) ? 1 : 0)) * lPageSize;
    lScratchChunkSizeMultiplier = ((lScratchChunkSizeMultiplier / lPageSize) + ((lScratchChunkSizeMultiplier % lPageSize) ? 1 : 0)) * lPageSize;

    mCudaChunkCollection.SetChunkSizeMultiplier(lCudaChunkSizeMultiplier);
    mScratchChunkCollection.SetChunkSizeMultiplier(SCRATCH_CHUNK_SIZE_MULTIPLIER_PER_GB * lGBs);

#ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
    size_t lPinnedChunkSizeMultiplier = PINNED_CHUNK_SIZE_MULTIPLIER_PER_GB * lGBs;
    lPinnedChunkSizeMultiplier = ((lPinnedChunkSizeMultiplier / lPageSize) + ((lPinnedChunkSizeMultiplier % lPageSize) ? 1 : 0)) * lPageSize;

    mPinnedChunkCollection.SetChunkSizeMultiplier(PINNED_CHUNK_SIZE_MULTIPLIER_PER_GB * lGBs);
#endif
}
    
const std::map<ulong, std::vector<pmCudaSubtaskMemoryStruct>>& pmStubCUDA::GetSubtaskPointersMap() const
{
    return mSubtaskPointersMap;
}
    
const std::map<ulong, pmCudaSubtaskSecondaryBuffersStruct>& pmStubCUDA::GetSubtaskSecondaryBuffersMap() const
{
    return mSubtaskSecondaryBuffersMap;
}

#ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
void pmStubCUDA::CopyDataToPinnedBuffers(pmTask* pTask, ulong pSubtaskId, pmSplitInfo* pSplitInfo, const pmSubtaskInfo& pSubtaskInfo)
{
    pmSubscriptionManager& lSubscriptionManager = pTask->GetSubscriptionManager();

    uint lAddressSpaceCount = pTask->GetAddressSpaceCount();
    std::vector<pmCudaSubtaskMemoryStruct>& lVector = mSubtaskPointersMap[pSubtaskId];
    
    DEBUG_EXCEPTION_ASSERT(lVector.size() <= lAddressSpaceCount + 1);

    for_each_with_index(pTask->GetAddressSpaces(), [&] (const pmAddressSpace* pAddressSpace, size_t pAddressSpaceIndex)
    {
        if(lVector[pAddressSpaceIndex].requiresLoad)
        {
            pmSubscriptionVisibilityType lVisibilityType = pTask->GetAddressSpaceSubscriptionVisibility(pAddressSpace, this);

            subscription::subscriptionRecordType::const_iterator lBegin, lEnd, lIter;
            lSubscriptionManager.GetNonConsolidatedReadSubscriptions(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex, lBegin, lEnd);

            if(lVisibilityType == SUBSCRIPTION_NATURAL)
            {
                pmSubscriptionInfo lSubscriptionInfo;

                if(pTask->IsReadOnly(pAddressSpace))
                    lSubscriptionInfo = lSubscriptionManager.GetConsolidatedReadSubscription(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex);
                else
                    lSubscriptionInfo = lSubscriptionManager.GetUnifiedReadWriteSubscription(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex);

                if(lSubscriptionInfo.length)
                {
                    for(lIter = lBegin; lIter != lEnd; ++lIter)
                    {
                        void* lPinnedPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(lVector[pAddressSpaceIndex].pinnedPtr) + lIter->first - lSubscriptionInfo.offset);
                        void* lDataPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(pSubtaskInfo.memInfo[pAddressSpaceIndex].ptr) + lIter->first - lSubscriptionInfo.offset);

                        memcpy(lPinnedPtr, lDataPtr, lIter->second.first);
                    }
                }
            }
            else    // SUBSCRIPTION_COMPACT
            {
                const subscription::pmCompactViewData& lCompactViewData = lSubscriptionManager.GetCompactedSubscription(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex);
                
                if(lCompactViewData.subscriptionInfo.length)
                {
                    void* lShadowMemAddr = pSubtaskInfo.memInfo[pAddressSpaceIndex].ptr;
                    
                    if(lShadowMemAddr && pTask->IsReadOnly(pAddressSpace))
                    {
                        memcpy(lVector[pAddressSpaceIndex].pinnedPtr, lShadowMemAddr, lCompactViewData.subscriptionInfo.length);
                    }
                    else
                    {
                        size_t lBaseAddr = reinterpret_cast<size_t>(pAddressSpace->GetMem());
                        size_t lShadowAddr = reinterpret_cast<size_t>(lShadowMemAddr);

                        auto lOffsetsIter = lCompactViewData.nonConsolidatedReadSubscriptionOffsets.begin();
                        DEBUG_EXCEPTION_ASSERT(std::distance(lOffsetsIter, lCompactViewData.nonConsolidatedReadSubscriptionOffsets.end()) == std::distance(lBegin, lEnd));

                        for(lIter = lBegin; lIter != lEnd; ++lIter, ++lOffsetsIter)
                        {
                            void* lPinnedPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(lVector[pAddressSpaceIndex].pinnedPtr) + (*lOffsetsIter));
                            void* lDataPtr = reinterpret_cast<void*>((lShadowMemAddr ? (lShadowAddr + (*lOffsetsIter)) : (lBaseAddr + lIter->first)));

                            memcpy(lPinnedPtr, lDataPtr, lIter->second.first);
                        }
                    }
                }
            }
        }
    });

    if(lVector.size() > lAddressSpaceCount)
    {
        pmScratchBufferType lScratchBufferType = SUBTASK_TO_POST_SUBTASK;
        size_t lScratchBufferSize = 0;
        void* lCpuScratchBuffer = lSubscriptionManager.CheckAndGetScratchBuffer(this, pSubtaskId, pSplitInfo, lScratchBufferSize, lScratchBufferType);
        if(lCpuScratchBuffer && lScratchBufferSize)
        {
            if(lScratchBufferType == PRE_SUBTASK_TO_SUBTASK || lScratchBufferType == PRE_SUBTASK_TO_POST_SUBTASK)
                memcpy(lVector.back().pinnedPtr, lCpuScratchBuffer, lScratchBufferSize);
        }
    }
    
    pmCudaSubtaskSecondaryBuffersStruct& lStruct = mSubtaskSecondaryBuffersMap[pSubtaskId];

    if(lStruct.statusPinnedPtr)
        *((pmStatus*)lStruct.statusPinnedPtr) = pmStatusUnavailable;
}
    
pmStatus pmStubCUDA::CopyDataFromPinnedBuffers(pmTask* pTask, ulong pSubtaskId, pmSplitInfo* pSplitInfo, const pmSubtaskInfo& pSubtaskInfo)
{
    pmSubscriptionManager& lSubscriptionManager = pTask->GetSubscriptionManager();
    
    uint lAddressSpaceCount = pTask->GetAddressSpaceCount();
    std::vector<pmCudaSubtaskMemoryStruct>& lVector = mSubtaskPointersMap[pSubtaskId];
    
    DEBUG_EXCEPTION_ASSERT(lVector.size() <= lAddressSpaceCount + 1);

    for_each_with_index(pTask->GetAddressSpaces(), [&] (const pmAddressSpace* pAddressSpace, size_t pAddressSpaceIndex)
    {
        if(!pTask->IsReadOnly(pAddressSpace))
        {
            pmSubscriptionVisibilityType lVisibilityType = pTask->GetAddressSpaceSubscriptionVisibility(pAddressSpace, this);
            
            subscription::subscriptionRecordType::const_iterator lBegin, lEnd, lIter;
            lSubscriptionManager.GetNonConsolidatedWriteSubscriptions(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex, lBegin, lEnd);

            if(lVisibilityType == SUBSCRIPTION_NATURAL)
            {
                pmSubscriptionInfo lSubscriptionInfo = lSubscriptionManager.GetUnifiedReadWriteSubscription(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex);
                
                if(lSubscriptionInfo.length)
                {
                    for(lIter = lBegin; lIter != lEnd; ++lIter)
                    {
                        void* lPinnedPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(lVector[pAddressSpaceIndex].pinnedPtr) + lIter->first - lSubscriptionInfo.offset);
                        void* lDataPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(pSubtaskInfo.memInfo[pAddressSpaceIndex].ptr) + lIter->first - lSubscriptionInfo.offset);

                        memcpy(lDataPtr, lPinnedPtr, lIter->second.first);
                    }
                }
            }
            else    // SUBSCRIPTION_COMPACT
            {
                const subscription::pmCompactViewData& lCompactViewData = lSubscriptionManager.GetCompactedSubscription(this, pSubtaskId, pSplitInfo, pAddressSpaceIndex);

                if(lCompactViewData.subscriptionInfo.length)
                {
                    void* lShadowMemAddr = pSubtaskInfo.memInfo[pAddressSpaceIndex].ptr;
                    
                    if(lShadowMemAddr && pTask->IsWriteOnly(pAddressSpace))
                    {
                        memcpy(lShadowMemAddr, lVector[pAddressSpaceIndex].pinnedPtr, lCompactViewData.subscriptionInfo.length);
                    }
                    else
                    {
                        size_t lBaseAddr = reinterpret_cast<size_t>(pAddressSpace->GetMem());
                        size_t lShadowAddr = reinterpret_cast<size_t>(lShadowMemAddr);

                        auto lOffsetsIter = lCompactViewData.nonConsolidatedWriteSubscriptionOffsets.begin();
                        DEBUG_EXCEPTION_ASSERT(std::distance(lOffsetsIter, lCompactViewData.nonConsolidatedWriteSubscriptionOffsets.end()) == std::distance(lBegin, lEnd));

                        for(lIter = lBegin; lIter != lEnd; ++lIter, ++lOffsetsIter)
                        {
                            void* lPinnedPtr = reinterpret_cast<void*>(reinterpret_cast<size_t>(lVector[pAddressSpaceIndex].pinnedPtr) + (*lOffsetsIter));
                            void* lDataPtr = reinterpret_cast<void*>((lShadowMemAddr ? (lShadowAddr + (*lOffsetsIter)) : (lBaseAddr + lIter->first)));

                            memcpy(lDataPtr, lPinnedPtr, lIter->second.first);
                        }
                    }
                }
            }
        }
    });
    
    if(lVector.size() > lAddressSpaceCount)
    {
        pmScratchBufferType lScratchBufferType = SUBTASK_TO_POST_SUBTASK;
        size_t lScratchBufferSize = 0;
        void* lCpuScratchBuffer = lSubscriptionManager.CheckAndGetScratchBuffer(this, pSubtaskId, pSplitInfo, lScratchBufferSize, lScratchBufferType);
        if(lCpuScratchBuffer && lScratchBufferSize)
        {
            if(lScratchBufferType == SUBTASK_TO_POST_SUBTASK || lScratchBufferType == PRE_SUBTASK_TO_POST_SUBTASK)
                memcpy(lCpuScratchBuffer, lVector.back().pinnedPtr, lScratchBufferSize);
        }
    }

    pmCudaSubtaskSecondaryBuffersStruct& lStruct = mSubtaskSecondaryBuffersMap[pSubtaskId];

    if(lStruct.statusPinnedPtr)
        return *((pmStatus*)lStruct.statusPinnedPtr);
    
    return pmStatusUnavailable;
}
#endif  // SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP

void pmStubCUDA::PrepareForSubtaskRangeExecution(pmTask* pTask, ulong pStartSubtaskId, ulong pEndSubtaskId, pmSplitInfo* pSplitInfo)
{
    DEBUG_EXCEPTION_ASSERT(!pSplitInfo || pStartSubtaskId == pEndSubtaskId);

#ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
    EXCEPTION_ASSERT(pEndSubtaskId - pStartSubtaskId + 1 == mSubtaskPointersMap.size());
#else
    EXCEPTION_ASSERT(pStartSubtaskId == pEndSubtaskId && mSubtaskPointersMap.size() == 1);
#endif
    
    mStartSubtaskId = pStartSubtaskId;

    DEBUG_EXCEPTION_ASSERT(mCudaStreams.size() == (pEndSubtaskId - pStartSubtaskId + 1));
}
    
void pmStubCUDA::WaitForSubtaskExecutionToFinish(pmTask* pTask, ulong pRangeStartSubtaskId, ulong pSubtaskId, pmSplitInfo* pSplitInfo)
{
    pmCudaInterface::WaitForStreamCompletion(*mCudaStreams[pSubtaskId - pRangeStartSubtaskId].get());
    
#ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
    CopyDataFromPinnedBuffers(pTask, pSubtaskId, pSplitInfo, pTask->GetSubscriptionManager().GetSubtaskInfo(this, pSubtaskId, pSplitInfo));
#endif
}

void pmStubCUDA::CleanupPostSubtaskRangeExecution(pmTask* pTask, ulong pStartSubtaskId, ulong pEndSubtaskId, ulong pCleanupEndSubtaskId, pmSplitInfo* pSplitInfo)
{
    DEBUG_EXCEPTION_ASSERT(pCleanupEndSubtaskId >= pEndSubtaskId);
    DEBUG_EXCEPTION_ASSERT(!pSplitInfo || pStartSubtaskId == pEndSubtaskId);
    DEBUG_EXCEPTION_ASSERT(mCudaStreams.size() == (pCleanupEndSubtaskId - pStartSubtaskId + 1));

    if(pTask->IsCudaCacheEnabled())
    {
    #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
        for(ulong subtaskId = pStartSubtaskId; subtaskId <= pCleanupEndSubtaskId; ++subtaskId)
        {
            std::vector<pmCudaSubtaskMemoryStruct>& lCudaSubtaskMemoryVector = mSubtaskPointersMap[subtaskId];
            
            filtered_for_each(lCudaSubtaskMemoryVector, [] (pmCudaSubtaskMemoryStruct& pStruct) {return pStruct.isUncached;}, [&] (pmCudaSubtaskMemoryStruct& pStruct)
            {
                mCudaChunkCollection.Deallocate(pStruct.cudaPtr);
            });
        }
    #endif
        
        for(ulong subtaskId = pEndSubtaskId + 1; subtaskId <= pCleanupEndSubtaskId; ++subtaskId)
        {
            for_each(mCacheKeys[subtaskId], [&] (const pmCudaCacheKey& pKey)
            {
                mCudaCache.RemoveKey(pKey);
            });
        }

        mCacheKeys.clear();
    }
    else
    {
    #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
        for(ulong subtaskId = pStartSubtaskId; subtaskId <= pCleanupEndSubtaskId; ++subtaskId)
        {
            std::vector<pmCudaSubtaskMemoryStruct>& lCudaSubtaskMemoryVector = mSubtaskPointersMap[subtaskId];
            
            filtered_for_each(lCudaSubtaskMemoryVector, [] (pmCudaSubtaskMemoryStruct& pStruct) {return pStruct.requiresLoad;}, [&] (pmCudaSubtaskMemoryStruct& pStruct)
            {
                mCudaChunkCollection.Deallocate(pStruct.cudaPtr);
            });
        }
    #endif
    }

    uint lAddressSpaceCount = pTask->GetAddressSpaceCount();
    
    for(ulong subtaskId = pStartSubtaskId; subtaskId <= pCleanupEndSubtaskId; ++subtaskId)
    {
        std::vector<pmCudaSubtaskMemoryStruct>& lCudaSubtaskMemoryVector = mSubtaskPointersMap[subtaskId];
        
    #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
        filtered_for_each(lCudaSubtaskMemoryVector, [] (pmCudaSubtaskMemoryStruct& pStruct) {return pStruct.requiresLoad;}, [&] (pmCudaSubtaskMemoryStruct& pStruct)
        {
            mPinnedChunkCollection.Deallocate(pStruct.pinnedPtr);
        });
    #endif
        
        // Deallocate scratch buffer from device
        if(lCudaSubtaskMemoryVector.size() > lAddressSpaceCount)
            mScratchChunkCollection.Deallocate(lCudaSubtaskMemoryVector.back().cudaPtr);

        pmCudaSubtaskSecondaryBuffersStruct& lSecondaryBuffersStruct = mSubtaskSecondaryBuffersMap[subtaskId];

        if(lSecondaryBuffersStruct.reservedMemCudaPtr)   // Reserved mem and status are allocated as a single entity
            mScratchChunkCollection.Deallocate(lSecondaryBuffersStruct.reservedMemCudaPtr);
        else
            mScratchChunkCollection.Deallocate(lSecondaryBuffersStruct.statusCudaPtr);

    #ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
        mPinnedChunkCollection.Deallocate(lSecondaryBuffersStruct.statusPinnedPtr);
    #endif
    }

    mSubtaskPointersMap.clear();
    mSubtaskSecondaryBuffersMap.clear();
    
    mCudaStreams.clear();
}
    
void pmStubCUDA::TerminateUserModeExecution()
{
}
    
#endif


bool execEventMatchFunc(const stubEvent& pEvent, void* pCriterion)
{
	if(pEvent.eventId == SUBTASK_EXEC)
    {
        const subtaskExecEvent& lEvent = static_cast<const subtaskExecEvent&>(pEvent);
        if(lEvent.range.task == (pmTask*)pCriterion)
            return true;
    }

	return false;
}

#ifdef SUPPORT_SPLIT_SUBTASKS
bool splitSubtaskCheckEventMatchFunc(const stubEvent& pEvent, void* pCriterion)
{
	if(pEvent.eventId == SPLIT_SUBTASK_CHECK)
    {
        const splitSubtaskCheckEvent& lEvent = static_cast<const splitSubtaskCheckEvent&>(pEvent);
        if(lEvent.task == (pmTask*)pCriterion)
            return true;
    }

	return false;
}
#endif

/* struct currentSubtaskRnageTerminus */
pmExecutionStub::currentSubtaskRangeTerminus::currentSubtaskRangeTerminus(bool& pReassigned, bool& pForceAckFlag, bool& pPrematureTermination, pmExecutionStub* pStub)
    : mReassigned(pReassigned)
    , mForceAckFlag(pForceAckFlag)
    , mPrematureTermination(pPrematureTermination)
    , mStub(pStub)
{
}

void pmExecutionStub::currentSubtaskRangeTerminus::Terminating(currentSubtaskRangeStats* pStats)
{
    mPrematureTermination = pStats->prematureTermination;
    mReassigned = pStats->reassigned;
    mForceAckFlag = pStats->forceAckFlag;

    if(pStats->prematureTermination)
    {
        if(pStats->taskListeningOnCancellation)
            pStats->task->RegisterStubCancellationMessage();
    }

    DEBUG_EXCEPTION_ASSERT(!mReassigned || pStats->originalAllottee);
}

#ifdef DUMP_EVENT_TIMELINE
std::string pmExecutionStub::GetEventTimelineName()
{
    std::stringstream lStream;
    lStream << "Device " << GetProcessingElement()->GetGlobalDeviceIndex();
    
    return lStream.str();
}
#endif

/* struct stubEvent */
bool execStub::stubEvent::BlocksSecondaryOperations()
{
    return (eventId == SUBTASK_EXEC);
}
    
}


