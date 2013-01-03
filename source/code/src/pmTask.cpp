
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

#include "pmTask.h"
#include "pmDevicePool.h"
#include "pmCallback.h"
#include "pmCallbackUnit.h"
#include "pmCluster.h"
#include "pmCommand.h"
#include "pmHardware.h"
#include "pmTaskManager.h"
#include "pmSubtaskManager.h"
#include "pmReducer.h"
#include "pmRedistributor.h"
#include "pmMemSection.h"
#include "pmTimedEventManager.h"
#include "pmStubManager.h"
#include "pmMemoryManager.h"

#include <vector>
#include <algorithm>

namespace pm
{

RESOURCE_LOCK_IMPLEMENTATION_CLASS pmLocalTask::mSequenceLock;
ulong pmLocalTask::mSequenceId = 0;
    
#define SAFE_GET_DEVICE_POOL(x) { x = pmDevicePool::GetDevicePool(); if(!x) PMTHROW(pmFatalErrorException()); }

/* class pmTask */
pmTask::pmTask(void* pTaskConf, uint pTaskConfLength, ulong pTaskId, pmMemSection* pMemRO, pmMemSection* pMemRW, pmMemInfo pInputMemInfo, pmMemInfo pOutputMemInfo, ulong pSubtaskCount, pmCallbackUnit* pCallbackUnit, uint pAssignedDeviceCount, pmMachine* pOriginatingHost, pmCluster* pCluster, ushort pPriority, scheduler::schedulingModel pSchedulingModel, bool pMultiAssignEnabled)
	: mOriginatingHost(pOriginatingHost)
    , mCluster(pCluster)
    , mSubscriptionManager(this)
    , mMultiAssignEnabled(pMultiAssignEnabled)
    , mReadOnlyMemAddrForSubtasks(NULL)
    , mAllStubsScannedForCancellationMessages(false)
    , mAllStubsScannedForShadowMemCommitMessages(false)
    , mOutstandingStubsForCancellationMessages(0)
    , mOutstandingStubsForShadowMemCommitMessages(0)
    , mCollectiveShadowMem(NULL)
    , mIndividualShadowMemAllocationLength(0)
    , mShadowMemCount(0)
{
	mTaskConf = pTaskConf;
	mTaskConfLength = pTaskConfLength;
	mTaskId = pTaskId;
	mMemRO = pMemRO;
	mMemRW = pMemRW;
	mCallbackUnit = pCallbackUnit;
	mSubtaskCount = pSubtaskCount;
	mAssignedDeviceCount = pAssignedDeviceCount;
	mSchedulingModel = pSchedulingModel;

	if(pPriority < MAX_PRIORITY_LEVEL)
		mPriority = MAX_PRIORITY_LEVEL;
	else
		mPriority = pPriority;

	mTaskInfo.taskHandle = NULL;
	mSubtaskExecutionFinished = false;
	mSubtasksExecuted = 0;
    
    if(pMemRO)
    {
        pMemRO->Lock(this, pInputMemInfo);
    
        if(pMemRO->IsLazy())
            mReadOnlyMemAddrForSubtasks = mMemRO->GetReadOnlyLazyMemoryMapping();
        else
            mReadOnlyMemAddrForSubtasks = mMemRO->GetMem();
    }
    
    if(pMemRW)
        pMemRW->Lock(this, pOutputMemInfo);
}

pmTask::~pmTask()
{
    mSubscriptionManager.DropAllSubscriptions();
}

pmStatus pmTask::FlushMemoryOwnerships()
{
    if(mMemRW && mMemRW->GetMem())
        mMemRW->FlushOwnerships();

    return pmSuccess;
}
    
void pmTask::UnlockMemories()
{
    if(mMemRO)
        mMemRO->Unlock(this);
    
    if(mMemRW)
        mMemRW->Unlock(this);
}
    
bool pmTask::IsMultiAssignEnabled()
{
    return mMultiAssignEnabled;
}

void* pmTask::GetTaskConfiguration()
{
	return mTaskConf;
}

uint pmTask::GetTaskConfigurationLength()
{
	return mTaskConfLength;
}

ulong pmTask::GetTaskId()
{
	return mTaskId;
}

pmMemSection* pmTask::GetMemSectionRO()
{
	return mMemRO;
}

pmMemSection* pmTask::GetMemSectionRW()
{
	return mMemRW;
}

pmCallbackUnit* pmTask::GetCallbackUnit()
{
	return mCallbackUnit;
}

ulong pmTask::GetSubtaskCount()
{
	return mSubtaskCount;
}

pmMachine* pmTask::GetOriginatingHost()
{
	return mOriginatingHost;
}

pmCluster* pmTask::GetCluster()
{
	return mCluster;
}

ushort pmTask::GetPriority()
{
	return mPriority;
}

uint pmTask::GetAssignedDeviceCount()
{
	return mAssignedDeviceCount;
}

scheduler::schedulingModel pmTask::GetSchedulingModel()
{
	return mSchedulingModel;
}

pmTaskExecStats& pmTask::GetTaskExecStats()
{
	return mTaskExecStats;
}

pmStatus pmTask::RandomizeDevices(std::vector<pmProcessingElement*>& pDevices)
{
	std::random_shuffle(pDevices.begin(), pDevices.end());

	return pmSuccess;
}

std::vector<pmProcessingElement*>& pmTask::GetStealListForDevice(pmProcessingElement* pDevice)
{
    FINALIZE_RESOURCE_PTR(dStealListLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mStealListLock, Lock(), Unlock());

    if(mStealListForDevice.find(pDevice) == mStealListForDevice.end())
    {
        std::vector<pmProcessingElement*>& lDevices = (dynamic_cast<pmLocalTask*>(this) != NULL) ? (((pmLocalTask*)this)->GetAssignedDevices()) : (((pmRemoteTask*)this)->GetAssignedDevices());

        std::srand((uint)reinterpret_cast<size_t>(pDevice));
        mStealListForDevice[pDevice] = lDevices;
        RandomizeDevices(mStealListForDevice[pDevice]);
    }
    
    return mStealListForDevice[pDevice];    
}

pmStatus pmTask::BuildTaskInfo()
{
	mTaskInfo.taskHandle = (void*)this;
	mTaskInfo.taskConf = GetTaskConfiguration();
	mTaskInfo.taskConfLength = GetTaskConfigurationLength();
	mTaskInfo.taskId = GetTaskId();
	mTaskInfo.subtaskCount = GetSubtaskCount();
	mTaskInfo.priority = GetPriority();
	mTaskInfo.originatingHost = *(GetOriginatingHost());

	return pmSuccess;
}

pmTaskInfo& pmTask::GetTaskInfo()
{
	if(!mTaskInfo.taskHandle)
		BuildTaskInfo();

	return mTaskInfo;
}

pmStatus pmTask::GetSubtaskInfo(pmExecutionStub* pStub, ulong pSubtaskId, pmSubtaskInfo& pSubtaskInfo, bool& pOutputMemWriteOnly)
{
	pmSubscriptionInfo lInputMemSubscriptionInfo, lOutputMemSubscriptionInfo;
	void* lOutputMem;

	pSubtaskInfo.subtaskId = pSubtaskId;
	if(mMemRO && mReadOnlyMemAddrForSubtasks && mSubscriptionManager.GetInputMemSubscriptionForSubtask(pStub, pSubtaskId, lInputMemSubscriptionInfo))
	{
		pSubtaskInfo.inputMem = (reinterpret_cast<char*>(mReadOnlyMemAddrForSubtasks) + lInputMemSubscriptionInfo.offset);
		pSubtaskInfo.inputMemLength = lInputMemSubscriptionInfo.length;
	}
	else
	{
		pSubtaskInfo.inputMem = NULL;
		pSubtaskInfo.inputMemLength = 0;
	}

	if(mMemRW && (lOutputMem = mMemRW->GetMem()) && mSubscriptionManager.GetUnifiedOutputMemSubscriptionForSubtask(pStub, pSubtaskId, lOutputMemSubscriptionInfo))
	{
		if(DoSubtasksNeedShadowMemory())
			pSubtaskInfo.outputMem = mSubscriptionManager.GetSubtaskShadowMem(pStub, pSubtaskId);
		else
			pSubtaskInfo.outputMem = (reinterpret_cast<char*>(lOutputMem) + lOutputMemSubscriptionInfo.offset);

		pSubtaskInfo.outputMemLength = lOutputMemSubscriptionInfo.length;
        pOutputMemWriteOnly = (mMemRW->GetMemInfo() == OUTPUT_MEM_WRITE_ONLY);
    
        if(pOutputMemWriteOnly)
        {
            pSubtaskInfo.outputMemWrite = pSubtaskInfo.outputMem;
            pSubtaskInfo.outputMemWriteLength = pSubtaskInfo.outputMemLength;
        }
        else
        {
            pmSubscriptionInfo lReadSubscriptionInfo, lWriteSubscriptionInfo;
            mSubscriptionManager.GetOutputMemSubscriptionForSubtask(pStub, pSubtaskId, true, lReadSubscriptionInfo);
            mSubscriptionManager.GetOutputMemSubscriptionForSubtask(pStub, pSubtaskId, false, lWriteSubscriptionInfo);
        
            pSubtaskInfo.outputMemRead = reinterpret_cast<void*>(reinterpret_cast<size_t>(pSubtaskInfo.outputMem) + lReadSubscriptionInfo.offset - lOutputMemSubscriptionInfo.offset);
            pSubtaskInfo.outputMemWrite = reinterpret_cast<void*>(reinterpret_cast<size_t>(pSubtaskInfo.outputMem) + lWriteSubscriptionInfo.offset - lOutputMemSubscriptionInfo.offset);
        
            pSubtaskInfo.outputMemReadLength = lReadSubscriptionInfo.length;
            pSubtaskInfo.outputMemWriteLength = lWriteSubscriptionInfo.length;
        }
	}
	else
	{
		pSubtaskInfo.outputMem = pSubtaskInfo.outputMemRead = pSubtaskInfo.outputMemWrite = NULL;
		pSubtaskInfo.outputMemLength = pSubtaskInfo.outputMemReadLength = pSubtaskInfo.outputMemWriteLength = 0;
        pOutputMemWriteOnly = false;
	}

	return pmSuccess;
}

void* pmTask::CheckOutSubtaskMemory(size_t pLength)
{
    bool lIsLazy = GetMemSectionRW()->IsLazy();

    FINALIZE_RESOURCE_PTR(dCollectiveShadowMemLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCollectiveShadowMemLock, Lock(), Unlock());

    if(mCollectiveShadowMem)
    {
        if(pLength > mIndividualShadowMemAllocationLength)
            return NULL;
    }
    else
    {
        if(!mUnallocatedShadowMemPool.empty())
            PMTHROW(pmFatalErrorException());

        size_t lPageCount = 0;
        mIndividualShadowMemAllocationLength = MEMORY_MANAGER_IMPLEMENTATION_CLASS::GetMemoryManager()->FindAllocationSize(pLength, lPageCount);
        
        mShadowMemCount = std::min(pmStubManager::GetStubManager()->GetStubCount(), GetSubtaskCount());
        size_t lTotalMemSize = mShadowMemCount * mIndividualShadowMemAllocationLength;
        
        mCollectiveShadowMem = MEMORY_MANAGER_IMPLEMENTATION_CLASS::GetMemoryManager()->CreateCheckOutMemory(lTotalMemSize, lIsLazy);
    
        void* lAddr = mCollectiveShadowMem;
        for(size_t i=0; i<mShadowMemCount; ++i)
        {
            mUnallocatedShadowMemPool.push_back(lAddr);
            lAddr = reinterpret_cast<void*>(reinterpret_cast<size_t>(lAddr) + mIndividualShadowMemAllocationLength);
        }
    }

    void* lMem = NULL;
    if(!mUnallocatedShadowMemPool.empty())
    {
        lMem = mUnallocatedShadowMemPool.back();
        mUnallocatedShadowMemPool.pop_back();

        if(lIsLazy)
            MEMORY_MANAGER_IMPLEMENTATION_CLASS::GetMemoryManager()->SetLazyProtection(lMem, mIndividualShadowMemAllocationLength, true, true);
    }
    
    return lMem;
}
    
void pmTask::RepoolCheckedOutSubtaskMemory(void* pMem)
{
    FINALIZE_RESOURCE_PTR(dCollectiveShadowMemLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCollectiveShadowMemLock, Lock(), Unlock());

#ifdef _DEBUG
    if(std::find(mUnallocatedShadowMemPool.begin(), mUnallocatedShadowMemPool.end(), pMem) != mUnallocatedShadowMemPool.end())
        PMTHROW(pmFatalErrorException());
    
    if(reinterpret_cast<size_t>(pMem) < reinterpret_cast<size_t>(mCollectiveShadowMem))
        PMTHROW(pmFatalErrorException());
    
    if(reinterpret_cast<size_t>(pMem) >= reinterpret_cast<size_t>(mCollectiveShadowMem) + (mShadowMemCount * mIndividualShadowMemAllocationLength))
        PMTHROW(pmFatalErrorException());
       
    if(((reinterpret_cast<size_t>(pMem) - reinterpret_cast<size_t>(mCollectiveShadowMem)) % mIndividualShadowMemAllocationLength) != 0)
        PMTHROW(pmFatalErrorException());
#endif
    
    mUnallocatedShadowMemPool.push_back(pMem);
}
    
pmSubscriptionManager& pmTask::GetSubscriptionManager()
{
	return mSubscriptionManager;
}

pmReducer* pmTask::GetReducer()
{
    FINALIZE_RESOURCE_PTR(dReducerLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mReducerLock, Lock(), Unlock());

	if(!mReducer.get_ptr())
		mReducer.reset(new pmReducer(this));

	return mReducer.get_ptr();
}

pmRedistributor* pmTask::GetRedistributor()
{
    FINALIZE_RESOURCE_PTR(dRedistributorLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mRedistributorLock, Lock(), Unlock());
    
    if(!mRedistributor.get_ptr())
        mRedistributor.reset(new pmRedistributor(this));
    
    return mRedistributor.get_ptr();
}
    
#ifdef ENABLE_TASK_PROFILING
pmTaskProfiler* pmTask::GetTaskProfiler()
{
    return &mTaskProfiler;
}
#endif

pmStatus pmTask::MarkSubtaskExecutionFinished()
{
	// Auto lock/unlock scope
	{
		FINALIZE_RESOURCE_PTR(dExecLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mExecLock, Lock(), Unlock());

		mSubtaskExecutionFinished = true;
	}

	if(mCallbackUnit->GetDataReductionCB())
		GetReducer()->CheckReductionFinish();

    if(mCallbackUnit->GetDataRedistributionCB())
        GetRedistributor()->SendRedistributionInfo();

	return pmSuccess;
}

bool pmTask::HasSubtaskExecutionFinished()
{
	FINALIZE_RESOURCE_PTR(dExecLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mExecLock, Lock(), Unlock());

	return mSubtaskExecutionFinished;
}

pmStatus pmTask::IncrementSubtasksExecuted(ulong pSubtaskCount)
{
	FINALIZE_RESOURCE_PTR(dExecLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mExecLock, Lock(), Unlock());

	mSubtasksExecuted += pSubtaskCount;

	return pmSuccess;
}

ulong pmTask::GetSubtasksExecuted()
{
	FINALIZE_RESOURCE_PTR(dExecLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mExecLock, Lock(), Unlock());

	return mSubtasksExecuted;
}

bool pmTask::DoSubtasksNeedShadowMemory()
{
    return (mMemRW != NULL);
//	return (mMemRW && (mMultiAssignEnabled || mMemRW->IsLazy() || mMemRW->IsReadWrite() || (mCallbackUnit->GetDataReductionCB() != NULL)));
}
    
void pmTask::TerminateTask()
{
}

void pmTask::RecordStubWillSendCancellationMessage()
{
	FINALIZE_RESOURCE_PTR(dTaskCompletionLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mTaskCompletionLock, Lock(), Unlock());

#ifdef _DEBUG
    if(mAllStubsScannedForCancellationMessages)
        PMTHROW(pmFatalErrorException());
#endif

    ++mOutstandingStubsForCancellationMessages;
}
    
void pmTask::MarkAllStubsScannedForCancellationMessages()
{
	FINALIZE_RESOURCE_PTR(dTaskCompletionLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mTaskCompletionLock, Lock(), Unlock());
    
    mAllStubsScannedForCancellationMessages = true;

    if(mOutstandingStubsForCancellationMessages == 0)
        MarkLocalStubsFreeOfCancellations();
}
    
void pmTask::RegisterStubCancellationMessage()
{
	FINALIZE_RESOURCE_PTR(dTaskCompletionLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mTaskCompletionLock, Lock(), Unlock());

#ifdef _DEBUG
    if(mOutstandingStubsForCancellationMessages == 0)
        PMTHROW(pmFatalErrorException());
#endif
    
    --mOutstandingStubsForCancellationMessages;

    if(mOutstandingStubsForCancellationMessages == 0 && mAllStubsScannedForCancellationMessages)
        MarkLocalStubsFreeOfCancellations();
}

void pmTask::RecordStubWillSendShadowMemCommitMessage()
{
	FINALIZE_RESOURCE_PTR(dTaskCompletionLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mTaskCompletionLock, Lock(), Unlock());

#ifdef _DEBUG
    if(mAllStubsScannedForShadowMemCommitMessages)
        PMTHROW(pmFatalErrorException());
#endif

    ++mOutstandingStubsForShadowMemCommitMessages;
}
    
void pmTask::MarkAllStubsScannedForShadowMemCommitMessages()
{
	FINALIZE_RESOURCE_PTR(dTaskCompletionLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mTaskCompletionLock, Lock(), Unlock());
    
    mAllStubsScannedForShadowMemCommitMessages = true;

    if(mOutstandingStubsForShadowMemCommitMessages == 0)
        MarkLocalStubsFreeOfShadowMemCommits();
}
    
void pmTask::RegisterStubShadowMemCommitMessage()
{
	FINALIZE_RESOURCE_PTR(dTaskCompletionLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mTaskCompletionLock, Lock(), Unlock());

#ifdef _DEBUG
    if(mOutstandingStubsForShadowMemCommitMessages == 0)
        PMTHROW(pmFatalErrorException());
#endif
    
    --mOutstandingStubsForShadowMemCommitMessages;

    if(mOutstandingStubsForShadowMemCommitMessages == 0 && mAllStubsScannedForShadowMemCommitMessages)
        MarkLocalStubsFreeOfShadowMemCommits();
}

void pmTask::MarkLocalStubsFreeOfCancellations()
{
}

void pmTask::MarkLocalStubsFreeOfShadowMemCommits()
{
}

ulong pmTask::GetSequenceNumber()
{
    return mSequenceNumber;
}
    
pmStatus pmTask::SetSequenceNumber(ulong pSequenceNumber)
{
    mSequenceNumber = pSequenceNumber;
    
    return pmSuccess;
}


/* class pmLocalTask */
pmLocalTask::pmLocalTask(void* pTaskConf, uint pTaskConfLength, ulong pTaskId, pmMemSection* pMemRO, pmMemSection* pMemRW, pmMemInfo pInputMemInfo, pmMemInfo pOutputMemInfo, ulong pSubtaskCount, pmCallbackUnit* pCallbackUnit, int pTaskTimeOutInSecs, pmMachine* pOriginatingHost /* = PM_LOCAL_MACHINE */, pmCluster* pCluster /* = PM_GLOBAL_CLUSTER */, ushort pPriority /* = DEFAULT_PRIORITY_LEVEL */, scheduler::schedulingModel pSchedulingModel /* =  DEFAULT_SCHEDULING_MODEL */, bool pMultiAssignEnabled /* = true */)
	: pmTask(pTaskConf, pTaskConfLength, pTaskId, pMemRO, pMemRW, pInputMemInfo, pOutputMemInfo, pSubtaskCount, pCallbackUnit, 0, pOriginatingHost, pCluster, pPriority, pSchedulingModel, pMultiAssignEnabled)
    , mPendingCompletions(0)
    , mUserSideTaskCompleted(false)
    , mLocalStubsFreeOfCancellations(false)
    , mLocalStubsFreeOfShadowMemCommits(false)
{
    ulong lCurrentTime = GetIntegralCurrentTimeInSecs();
    ulong lTaskTimeOutTriggerTime = lCurrentTime + pTaskTimeOutInSecs;
    if(pTaskTimeOutInSecs < 0 || lTaskTimeOutTriggerTime < lCurrentTime || lTaskTimeOutTriggerTime > (ulong)__MAX(int))
        mTaskTimeOutTriggerTime = (ulong)__MAX(int);
    
	mTaskCommand = pmTaskCommand::CreateSharedPtr(pPriority, pmTaskCommand::BASIC_TASK);
    
    // Auto lock/unlock scope
    {
        FINALIZE_RESOURCE(dSequenceLock, mSequenceLock.Lock(), mSequenceLock.Unlock());    
        SetSequenceNumber(mSequenceId);
        ++mSequenceId;
    }
}

pmLocalTask::~pmLocalTask()
{
}

void pmLocalTask::TerminateTask()
{
    pmTimedEventManager::GetTimedEventManager()->ClearTaskTimeOutEvent(this, GetTaskTimeOutTriggerTime());    
    pmScheduler::GetScheduler()->TerminateTaskEvent(this);
}
    
void pmLocalTask::RegisterInternalTaskCompletionMessage()
{
    FINALIZE_RESOURCE_PTR(dCompletionLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCompletionLock, Lock(), Unlock());
    --mPendingCompletions;

    if(mPendingCompletions == 0)
        DoPostInternalCompletion();
}

pmStatus pmLocalTask::MarkSubtaskExecutionFinished()
{
    pmTask::MarkSubtaskExecutionFinished();
   
    pmCallbackUnit* lCallbackUnit = GetCallbackUnit();
    if(!lCallbackUnit->GetDataReductionCB() && !lCallbackUnit->GetDataRedistributionCB())
        MarkUserSideTaskCompletion();

    return pmSuccess;
}

// This method must be called with mCompletionLock acquired
void pmLocalTask::DoPostInternalCompletion()
{
    FlushMemoryOwnerships();
    UnlockMemories();

    MarkTaskEnd(mSubtaskManager.get_ptr() ? mSubtaskManager->GetTaskExecutionStatus() : pmNoCompatibleDevice);
}
    
void pmLocalTask::TaskRedistributionDone(pmMemSection* pRedistributedMemSection)
{
    mMemRW = pRedistributedMemSection;
    MarkUserSideTaskCompletion();
}

void pmLocalTask::MarkLocalStubsFreeOfCancellations()
{
#ifdef _DEBUG
    if(!IsMultiAssignEnabled())
        PMTHROW(pmFatalErrorException());
#endif
    
    FINALIZE_RESOURCE_PTR(dCompletionLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCompletionLock, Lock(), Unlock());

    if(mUserSideTaskCompleted && (!GetMemSectionRW()->IsReadWrite() || mLocalStubsFreeOfShadowMemCommits))
        pmScheduler::GetScheduler()->SendTaskCompleteToTaskOwner(this);
    
    mLocalStubsFreeOfCancellations = true;
}

void pmLocalTask::MarkLocalStubsFreeOfShadowMemCommits()
{
#ifdef _DEBUG
    if(!GetMemSectionRW()->IsReadWrite())
        PMTHROW(pmFatalErrorException());
#endif
    
    FINALIZE_RESOURCE_PTR(dCompletionLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCompletionLock, Lock(), Unlock());

    if(mUserSideTaskCompleted && (!IsMultiAssignEnabled() || mLocalStubsFreeOfCancellations))
        pmScheduler::GetScheduler()->SendTaskCompleteToTaskOwner(this);
    
    mLocalStubsFreeOfShadowMemCommits = true;
}
    
void pmLocalTask::MarkUserSideTaskCompletion()
{
    bool lIsMultiAssign = IsMultiAssignEnabled();
    
    // Auto lock/unlock scope
    {
        FINALIZE_RESOURCE_PTR(dCompletionLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCompletionLock, Lock(), Unlock());

        if((!lIsMultiAssign || mLocalStubsFreeOfCancellations) && (!GetMemSectionRW()->IsReadWrite() || mLocalStubsFreeOfShadowMemCommits))
            pmScheduler::GetScheduler()->SendTaskCompleteToTaskOwner(this);
            
        mUserSideTaskCompleted = true;
    
        if(mPendingCompletions == 0)
            DoPostInternalCompletion();
    }
}

void pmLocalTask::UserDeleteTask()
{
    FINALIZE_RESOURCE_PTR(dCompletionLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCompletionLock, Lock(), Unlock());

    TerminateTask();
}
    
pmStatus pmLocalTask::SaveFinalReducedOutput(pmExecutionStub* pStub, ulong pSubtaskId)
{
    pmSubscriptionManager& lSubscriptionManager = GetSubscriptionManager();
	pmSubscriptionInfo lSubscriptionInfo;
	if(!lSubscriptionManager.GetOutputMemSubscriptionForSubtask(pStub, pSubtaskId, false, lSubscriptionInfo))
	{
		lSubscriptionManager.DestroySubtaskShadowMem(pStub, pSubtaskId);
		PMTHROW(pmFatalErrorException());
	}
std::cout << "Pending Implementation" << std::endl;
	void* lShadowMem = lSubscriptionManager.GetSubtaskShadowMem(pStub, pSubtaskId);
	GetMemSectionRW()->Update(lSubscriptionInfo.offset, lSubscriptionInfo.length, lShadowMem);
	lSubscriptionManager.DestroySubtaskShadowMem(pStub, pSubtaskId);

    ((pmLocalTask*)this)->MarkUserSideTaskCompletion();
    
    return pmSuccess;
}

pmStatus pmLocalTask::InitializeSubtaskManager(scheduler::schedulingModel pSchedulingModel)
{
	switch(pSchedulingModel)
	{
		case scheduler::PUSH:
			mSubtaskManager.reset(new pmPushSchedulingManager(this));
			break;

		case scheduler::PULL:
		case scheduler::STATIC_EQUAL:
			mSubtaskManager.reset(new pmPullSchedulingManager(this));
			break;

		case scheduler::STATIC_PROPORTIONAL:
			mSubtaskManager.reset(new pmProportionalSchedulingManager(this));
			break;

		default:
			PMTHROW(pmFatalErrorException());
	}

	return pmSuccess;
}
    
ulong pmLocalTask::GetTaskTimeOutTriggerTime()
{
    return mTaskTimeOutTriggerTime;
}

std::vector<pmProcessingElement*>& pmLocalTask::GetAssignedDevices()
{
	return mDevices;
}

pmStatus pmLocalTask::WaitForCompletion()
{
	return mTaskCommand->WaitForFinish();
}

double pmLocalTask::GetExecutionTimeInSecs()
{
	return mTaskCommand->GetExecutionTimeInSecs();
}

pmStatus pmLocalTask::MarkTaskStart()
{
	return mTaskCommand->MarkExecutionStart();
}

pmStatus pmLocalTask::MarkTaskEnd(pmStatus pStatus)
{
	return mTaskCommand->MarkExecutionEnd(pStatus, std::tr1::static_pointer_cast<pmCommand>(mTaskCommand));
}

pmStatus pmLocalTask::GetStatus()
{
	return mTaskCommand->GetStatus();
}

pmStatus pmLocalTask::FindCandidateProcessingElements(std::set<pmProcessingElement*>& pDevices)
{
	pmDevicePool* lDevicePool;
	SAFE_GET_DEVICE_POOL(lDevicePool);

	mDevices.clear();
	pDevices.clear();

	pmSubtaskCB* lSubtaskCB = GetCallbackUnit()->GetSubtaskCB();
	if(lSubtaskCB)
	{
		for(uint i=0; i<MAX_DEVICE_TYPES; ++i)
		{
			if(lSubtaskCB->IsCallbackDefinedForDevice((pmDeviceType)i))
				lDevicePool->GetAllDevicesOfTypeInCluster((pmDeviceType)i, GetCluster(), pDevices);
		}
	}

	if(!pDevices.empty())
	{
		pmDeviceSelectionCB* lDeviceSelectionCB = GetCallbackUnit()->GetDeviceSelectionCB();
		if(lDeviceSelectionCB)
		{
			std::set<pmProcessingElement*> lDevices;

			std::set<pmProcessingElement*>::iterator lIter;
			for(lIter = pDevices.begin(); lIter != pDevices.end(); ++lIter)
			{
				if(lDeviceSelectionCB->Invoke(this, *lIter))
					lDevices.insert(*lIter);
			}

			pDevices = lDevices;
		}
	}

	// If the number of subtasks is less than number of devices, then discard the extra devices
	ulong lSubtaskCount = GetSubtaskCount();
	ulong lDeviceCount = (ulong)(pDevices.size());	
	ulong lFinalCount = lDeviceCount;

	if(lSubtaskCount < lDeviceCount)
		lFinalCount = lSubtaskCount;

	std::set<pmProcessingElement*>::iterator lIter = pDevices.begin();
	for(ulong i=0; i<lFinalCount; ++i)
	{
		mDevices.push_back(*lIter);
		++lIter;
	}

	mAssignedDeviceCount = (uint)(mDevices.size());

    pDevices.erase(lIter, pDevices.end());

    if(!pDevices.empty())
    {
        std::set<pmMachine*> lMachines;
        pmProcessingElement::GetMachines(mDevices, lMachines);
        
        if(IsMultiAssignEnabled())
        {
            FINALIZE_RESOURCE_PTR(dCompletionLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCompletionLock, Lock(), Unlock());

            mPendingCompletions = lMachines.size();
            if(lMachines.find(PM_LOCAL_MACHINE) == lMachines.end())
                ++mPendingCompletions;
        }
    }

	return pmSuccess;
}

pmSubtaskManager* pmLocalTask::GetSubtaskManager()
{
	return mSubtaskManager.get_ptr();
}


/* class pmRemoteTask */
pmRemoteTask::pmRemoteTask(void* pTaskConf, uint pTaskConfLength, ulong pTaskId, pmMemSection* pMemRO, pmMemSection* pMemRW, pmMemInfo pInputMemInfo, pmMemInfo pOutputMemInfo, ulong pSubtaskCount, pmCallbackUnit* pCallbackUnit, uint pAssignedDeviceCount, pmMachine* pOriginatingHost, ulong pSequenceNumber, pmCluster* pCluster /* = PM_GLOBAL_CLUSTER */, ushort pPriority /* = DEFAULT_PRIORITY_LEVEL */, scheduler::schedulingModel pSchedulingModel /* =  DEFAULT_SCHEDULING_MODEL */, bool pMultiAssignEnabled /* = true */)
	: pmTask(pTaskConf, pTaskConfLength, pTaskId, pMemRO, pMemRW, pInputMemInfo, pOutputMemInfo, pSubtaskCount, pCallbackUnit, pAssignedDeviceCount, pOriginatingHost, pCluster, pPriority, pSchedulingModel, pMultiAssignEnabled)
    , mUserSideTaskCompleted(false)
    , mLocalStubsFreeOfCancellations(false)
    , mLocalStubsFreeOfShadowMemCommits(false)
{
    SetSequenceNumber(pSequenceNumber);
}

pmRemoteTask::~pmRemoteTask()
{
    FlushMemoryOwnerships();
    UnlockMemories();
}

pmStatus pmRemoteTask::AddAssignedDevice(pmProcessingElement* pDevice)
{
	mDevices.push_back(pDevice);

	uint lCount = GetAssignedDeviceCount();
	uint lSize = (uint)(mDevices.size());
	if(lSize > lCount)
		PMTHROW(pmFatalErrorException());

	return pmSuccess;
}

std::vector<pmProcessingElement*>& pmRemoteTask::GetAssignedDevices()
{
	return mDevices;
}
    
void pmRemoteTask::TerminateTask()
{
    SAFE_FREE(GetTaskConfiguration());
    pmScheduler::GetScheduler()->TerminateTaskEvent(this);
}

void pmRemoteTask::MarkLocalStubsFreeOfCancellations()
{
#ifdef _DEBUG
    if(!IsMultiAssignEnabled())
        PMTHROW(pmFatalErrorException());
#endif

    FINALIZE_RESOURCE_PTR(dCompletionLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCompletionLock, Lock(), Unlock());

    if(mUserSideTaskCompleted && (!GetMemSectionRW()->IsReadWrite() || mLocalStubsFreeOfShadowMemCommits))
    {
        pmScheduler::GetScheduler()->SendTaskCompleteToTaskOwner(this);
        TerminateTask();
    }
    else
    {
        mLocalStubsFreeOfCancellations = true;
    }
}

void pmRemoteTask::MarkLocalStubsFreeOfShadowMemCommits()
{
#ifdef _DEBUG
    if(!GetMemSectionRW()->IsReadWrite())
        PMTHROW(pmFatalErrorException());
#endif

    FINALIZE_RESOURCE_PTR(dCompletionLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCompletionLock, Lock(), Unlock());

    if(mUserSideTaskCompleted && (!IsMultiAssignEnabled() || mLocalStubsFreeOfCancellations))
    {
        pmScheduler::GetScheduler()->SendTaskCompleteToTaskOwner(this);
        TerminateTask();
    }
    else
    {
        mLocalStubsFreeOfShadowMemCommits = true;
    }
}

void pmRemoteTask::MarkUserSideTaskCompletion()
{
    bool lIsMultiAssign = IsMultiAssignEnabled();
    
    FINALIZE_RESOURCE_PTR(dCompletionLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mCompletionLock, Lock(), Unlock());

    if(!lIsMultiAssign && !GetMemSectionRW()->IsReadWrite())
    {
        TerminateTask();
    }
    else
    {
        if((!lIsMultiAssign || mLocalStubsFreeOfCancellations) && (!GetMemSectionRW()->IsReadWrite() || mLocalStubsFreeOfShadowMemCommits))
        {
            pmScheduler::GetScheduler()->SendTaskCompleteToTaskOwner(this);
            TerminateTask();
        }
        else
        {
            mUserSideTaskCompleted = true;
        }
    }
}

void pmRemoteTask::MarkReductionFinished()
{
    MarkUserSideTaskCompletion();
}
    
void pmRemoteTask::MarkRedistributionFinished()
{
    MarkUserSideTaskCompletion();
}
    
pmStatus pmRemoteTask::MarkSubtaskExecutionFinished()
{
    pmTask::MarkSubtaskExecutionFinished();
    
    pmCallbackUnit* lCallbackUnit = GetCallbackUnit();
    if(!lCallbackUnit->GetDataReductionCB() && !lCallbackUnit->GetDataRedistributionCB())
        MarkUserSideTaskCompletion();

    return pmSuccess;
}   

};

