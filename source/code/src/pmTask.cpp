
#include "pmTask.h"
#include "pmDevicePool.h"
#include "pmCallback.h"
#include "pmCallbackUnit.h"
#include "pmCluster.h"
#include "pmCommand.h"
#include "pmHardware.h"
#include "pmTaskManager.h"
#include "pmSubtaskManager.h"

#include <vector>
#include <algorithm>

namespace pm
{

#define SAFE_GET_DEVICE_POOL(x) { x = pmDevicePool::GetDevicePool(); if(!x) throw pmFatalErrorException(); }

/* class pmTask */
pmTask::pmTask(void* pTaskConf, uint pTaskConfLength, ulong pTaskId, pmMemSection* pMemRO, pmMemSection* pMemRW, ulong pSubtaskCount, pmCallbackUnit* pCallbackUnit, uint pAssignedDeviceCount /* = 0 */,
	pmMachine* pOriginatingHost /* = PM_LOCAL_MACHINE */, pmCluster* pCluster /* = PM_GLOBAL_CLUSTER */, ushort pPriority /* = MAX_PRIORITY_LEVEL */,
	pmScheduler::schedulingModel pSchedulingModel /* =  DEFAULT_SCHEDULING_MODEL */)
	: mOriginatingHost(pOriginatingHost), mCluster(pCluster), mSubscriptionManager(this)
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

	if(pPriority == 0)
		mPriority = 1;	// priority 0 is used for control messages only
	else
		mPriority = pPriority;

	mTaskInfo.taskHandle = NULL;
	mSubtaskExecutionFinished = false;
	mSubtasksExecuted = 0;
}

pmTask::~pmTask()
{
	if(mReducer)
		delete mReducer;

	if(mMemRO)
		mMemRO->FlushOwnerships();

	if(mMemRW)
		mMemRW->FlushOwnerships();
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

pmScheduler::schedulingModel pmTask::GetSchedulingModel()
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

pmStatus pmTask::GetSubtaskInfo(ulong pSubtaskId, pmSubtaskInfo& pSubtaskInfo)
{
	pmSubscriptionInfo lInputMemSubscriptionInfo, lOutputMemSubscriptionInfo;
	void* lInputMem;
	void* lOutputMem;

	pSubtaskInfo.subtaskId = pSubtaskId;
	if(mMemRO && (lInputMem = mMemRO->GetMem()) && mSubscriptionManager.GetInputMemSubscriptionForSubtask(pSubtaskId, lInputMemSubscriptionInfo))
	{
		pSubtaskInfo.inputMem = ((char*)lInputMem + lInputMemSubscriptionInfo.offset);
		pSubtaskInfo.inputMemLength = lInputMemSubscriptionInfo.length;
	}
	else
	{
		pSubtaskInfo.inputMem = NULL;
		pSubtaskInfo.inputMemLength = 0;
	}

	if(mMemRW && (lOutputMem = mMemRW->GetMem()) && mSubscriptionManager.GetInputMemSubscriptionForSubtask(pSubtaskId, lInputMemSubscriptionInfo))
	{
		if(DoSubtasksNeedShadowMemory())
			pSubtaskInfo.outputMem = GetSubtaskShadowMem(pSubtaskId).addr;
		else
			pSubtaskInfo.outputMem = ((char*)lOutputMem + lOutputMemSubscriptionInfo.offset);

		pSubtaskInfo.outputMemLength = lOutputMemSubscriptionInfo.length;
	}
	else
	{
		pSubtaskInfo.outputMem = NULL;
		pSubtaskInfo.outputMemLength = 0;
	}

	return pmSuccess;
}

pmSubscriptionManager& pmTask::GetSubscriptionManager()
{
	return mSubscriptionManager;
}

pmReducer* pmTask::GetReducer()
{
	if(!mReducer)
		mReducer = new pmReducer(this);

	return mReducer;
}

pmStatus pmTask::MarkSubtaskExecutionFinished()
{
	FINALIZE_RESOURCE_PTR(dExecLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mExecLock, Lock(), Unlock());

	mSubtaskExecutionFinished = true;

	if(mReducer)
		mReducer->CheckReductionFinish();

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
}

ulong pmTask::GetSubtasksExecuted()
{
	FINALIZE_RESOURCE_PTR(dExecLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mExecLock, Lock(), Unlock());

	return mSubtasksExecuted;
}

pmStatus pmTask::SaveFinalReducedOutput(ulong pSubtaskId)
{
	pmSubscriptionInfo lSubscriptionInfo;
	if(!mSubscriptionManager.GetOutputMemSubscriptionForSubtask(pSubtaskId, lSubscriptionInfo))
	{
		DestroySubtaskShadowMem(pSubtaskId);
		throw pmFatalErrorException();
	}

	subtaskShadowMem& lShadowMem = GetSubtaskShadowMem(pSubtaskId);
	(static_cast<pmOutputMemSection*>(mMemRW))->Update(lSubscriptionInfo.offset, lSubscriptionInfo.length, lShadowMem.addr);
	return DestroySubtaskShadowMem(pSubtaskId);
}

bool pmTask::DoSubtasksNeedShadowMemory()
{
	return (mCallbackUnit->GetDataReductionCB() != NULL);
}

pmStatus pmTask::CreateSubtaskShadowMem(ulong pSubtaskId)
{
	FINALIZE_RESOURCE_PTR(dShadowMemLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mShadowMemLock, Lock(), Unlock());

	pmSubscriptionInfo lSubscriptionInfo;

	if(mShadowMemMap.find(pSubtaskId) != mShadowMemMap.end() || !mSubscriptionManager.GetOutputMemSubscriptionForSubtask(pSubtaskId, lSubscriptionInfo))
		throw pmFatalErrorException();

	mShadowMemMap[pSubtaskId].addr = new char[lSubscriptionInfo.length];
	mShadowMemMap[pSubtaskId].length = lSubscriptionInfo.length;

	memcpy(mShadowMemMap[pSubtaskId].addr, (void*)((char*)(mMemRW->GetMem()) + lSubscriptionInfo.offset), lSubscriptionInfo.length);

	return pmSuccess;
}

pmStatus pmTask::CreateSubtaskShadowMem(ulong pSubtaskId, char* pMem, size_t pMemLength)
{
	FINALIZE_RESOURCE_PTR(dShadowMemLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mShadowMemLock, Lock(), Unlock());

	pmSubscriptionInfo lSubscriptionInfo;

	if(mShadowMemMap.find(pSubtaskId) != mShadowMemMap.end() || !mSubscriptionManager.GetOutputMemSubscriptionForSubtask(pSubtaskId, lSubscriptionInfo))
		throw pmFatalErrorException();

	mShadowMemMap[pSubtaskId].addr = new char[pMemLength];
	mShadowMemMap[pSubtaskId].length = pMemLength;

	memcpy(mShadowMemMap[pSubtaskId].addr, pMem, pMemLength);

	return pmSuccess;
}

pmTask::subtaskShadowMem& pmTask::GetSubtaskShadowMem(ulong pSubtaskId)
{
	FINALIZE_RESOURCE_PTR(dShadowMemLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mShadowMemLock, Lock(), Unlock());

	if(mShadowMemMap.find(pSubtaskId) == mShadowMemMap.end())
		throw pmFatalErrorException();
	
	return mShadowMemMap[pSubtaskId];
}

pmStatus pmTask::DestroySubtaskShadowMem(ulong pSubtaskId)
{
	FINALIZE_RESOURCE_PTR(dShadowMemLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mShadowMemLock, Lock(), Unlock());

	if(mShadowMemMap.find(pSubtaskId) == mShadowMemMap.end())
		throw pmFatalErrorException();

	delete[] mShadowMemMap[pSubtaskId].addr;

	return pmSuccess;
}


/* class pmLocalTask */
pmLocalTask::pmLocalTask(void* pTaskConf, uint pTaskConfLength, ulong pTaskId, pmMemSection* pMemRO, pmMemSection* pMemRW, ulong pSubtaskCount, pmCallbackUnit* pCallbackUnit, 
	pmMachine* pOriginatingHost = PM_LOCAL_MACHINE,	pmCluster* pCluster = PM_GLOBAL_CLUSTER, ushort pPriority = MAX_PRIORITY_LEVEL,
	pmScheduler::schedulingModel pSchedulingModel /* =  DEFAULT_SCHEDULING_MODEL */)
	: pmTask(pTaskConf, pTaskConfLength, pTaskId, pMemRO, pMemRW, pSubtaskCount, pCallbackUnit, 0, pOriginatingHost, pCluster, pPriority, pSchedulingModel)
{
	mSubtaskManager = NULL;
	mTaskCommand = pmTaskCommand::CreateSharedPtr(pPriority, pmTaskCommand::BASIC_TASK);
}

pmLocalTask::~pmLocalTask()
{
	pmTaskManager::GetTaskManager()->DeleteTask(this);

	if(mSubtaskManager)
		delete mSubtaskManager;
}

pmStatus pmLocalTask::InitializeSubtaskManager(pmScheduler::schedulingModel pSchedulingModel)
{
	switch(pSchedulingModel)
	{
		case pmScheduler::PUSH:
			mSubtaskManager = new pmPushSchedulingManager(this);
			break;

		case pmScheduler::PULL:
			mSubtaskManager = new pmPullSchedulingManager(this);
			break;

		default:
			throw pmFatalErrorException();
	}

	return pmSuccess;
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
	return mTaskCommand->MarkExecutionEnd(pStatus);
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
		for(uint i=0; i<pmDeviceTypes::MAX_DEVICE_TYPES; ++i)
		{
			if(lSubtaskCB->IsCallbackDefinedForDevice((pmDeviceTypes)i))
				lDevicePool->GetAllDevicesOfTypeInCluster((pmDeviceTypes)i, GetCluster(), pDevices);
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
				if(lDeviceSelectionCB->Invoke(this, lIter._Mynode()->_Myval))
					lDevices.insert(lIter._Mynode()->_Myval);
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
		mDevices.push_back(lIter._Mynode()->_Myval);
		++lIter;
	}

	mAssignedDeviceCount = mDevices.size();

	if(GetSchedulingModel() == pmScheduler::PULL)
		RandomizeDevices(mDevices);

	return pmSuccess;
}

pmSubtaskManager* pmLocalTask::GetSubtaskManager()
{
	return mSubtaskManager;
}


/* class pmRemoteTask */
pmRemoteTask::pmRemoteTask(void* pTaskConf, uint pTaskConfLength, ulong pTaskId, pmMemSection* pMemRO, pmMemSection* pMemRW, ulong pSubtaskCount, pmCallbackUnit* pCallbackUnit,
	uint pAssignedDeviceCount, pmMachine* pOriginatingHost, ulong pInternalTaskId, pmCluster* pCluster = PM_GLOBAL_CLUSTER, ushort pPriority = MAX_PRIORITY_LEVEL,
	pmScheduler::schedulingModel pSchedulingModel /* =  DEFAULT_SCHEDULING_MODEL */)
	: pmTask(pTaskConf, pTaskConfLength, pTaskId, pMemRO, pMemRW, pSubtaskCount, pCallbackUnit, pAssignedDeviceCount, pOriginatingHost, pCluster, pPriority, pSchedulingModel)
{
	mInternalTaskId = pInternalTaskId;
}

pmRemoteTask::~pmRemoteTask()
{
	pmTaskManager::GetTaskManager()->DeleteTask(this);
}

pmStatus pmRemoteTask::MarkSubtaskExecutionFinished()
{
	pmCallbackUnit* lCallbackUnit = GetCallbackUnit();
	if(!lCallbackUnit->GetDataReductionCB() && !lCallbackUnit->GetDataScatterCB())
	{
		pmTask::MarkSubtaskExecutionFinished();
		delete this;
	}
	else
	{
		pmTask::MarkSubtaskExecutionFinished();
	}
}

ulong pmRemoteTask::GetInternalTaskId()
{
	return mInternalTaskId;
}

pmStatus pmRemoteTask::AddAssignedDevice(pmProcessingElement* pDevice)
{
	mDevices.push_back(pDevice);

	uint lCount = GetAssignedDeviceCount();
	uint lSize = (uint)(mDevices.size());
	if(lSize > lCount)
		throw pmFatalErrorException();

	if(lSize == lCount && GetSchedulingModel() == pmScheduler::PULL)
		RandomizeDevices(mDevices);

	return pmSuccess;
}

std::vector<pmProcessingElement*>& pmRemoteTask::GetAssignedDevices()
{
	return mDevices;
}

};

