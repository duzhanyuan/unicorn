
#include "pmSubscriptionManager.h"
#include "pmCommunicator.h"
#include "pmTaskManager.h"
#include "pmMemoryManager.h"
#include "pmMemSection.h"
#include "pmTask.h"

namespace pm
{
    
using namespace subscription;

pmSubscriptionManager::pmSubscriptionManager(pmTask* pTask)
{
	mTask = pTask;
}

pmSubscriptionManager::~pmSubscriptionManager()
{
}

pmStatus pmSubscriptionManager::InitializeSubtaskDefaults(ulong pSubtaskId)
{
    FINALIZE_RESOURCE_PTR(dResourceLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mResourceLock, Lock(), Unlock());
    
    if(mSubtaskMap.find(pSubtaskId) != mSubtaskMap.end())
        PMTROW(pmFatalErrorException());

    pmSubtask lSubtask;
    mSubtaskMap[pSubtaskId] = lSubtask;

	return pmSuccess;
}

pmStatus pmSubscriptionManager::RegisterSubscription(ulong pSubtaskId, bool pIsInputMem, pmSubscriptionInfo pSubscriptionInfo)
{
    FINALIZE_RESOURCE_PTR(dResourceLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mResourceLock, Lock(), Unlock());

    if(mSubtaskMap.find(pSubtaskId) == mSubtaskMap.end())
        PMTROW(pmFatalErrorException());

	// Only one subscription allowed per subtask (for now)
	if(pIsInputMem)
	{
		if(!mTask->GetMemSectionRO())
            PMTROW(pmFatalErrorException());
        
        std::vector<pmSubscriptionInfo> lVector;
        lVector.push_back(pSubscriptionInfo);
        mSubtaskMap[pSubtaskId].mInputMemSubscriptions.first = lVector;
	}
	else
	{
		if(!mTask->GetMemSectionRW())
            PMTROW(pmFatalErrorException());

        std::vector<pmSubscriptionInfo> lVector;
        lVector.push_back(pSubscriptionInfo);
        mSubtaskMap[pSubtaskId].mOutputMemSubscriptions.first = lVector;
	}

	return pmSuccess;
}

pmStatus pmSubscriptionManager::SetCudaLaunchConfiguration(ulong pSubtaskId, pmCudaLaunchConf& pCudaLaunchConf)
{
    FINALIZE_RESOURCE_PTR(dResourceLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mResourceLock, Lock(), Unlock());

    if(mSubtaskMap.find(pSubtaskId) == mSubtaskMap.end())
        PMTROW(pmFatalErrorException());

    mSubtaskMap[pSubtaskId].mCudaLaunchConf = pCudaLaunchConf;
    
    return pmSuccess;
}
    
pmCudaLaunchConf& pmSubscriptionManager::GetCudaLaunchConf(ulong pSubtaskId)
{
    FINALIZE_RESOURCE_PTR(dResourceLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mResourceLock, Lock(), Unlock());
    
    if(mSubtaskMap.find(pSubtaskId) == mSubtaskMap.end())
        PMTROW(pmFatalErrorException());
    
    return mSubtaskMap[pSubtaskId].mCudaLaunchConf;
}

bool pmSubscriptionManager::GetInputMemSubscriptionForSubtask(ulong pSubtaskId, pmSubscriptionInfo& pSubscriptionInfo)
{
	FINALIZE_RESOURCE_PTR(dResourceLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mResourceLock, Lock(), Unlock());

    if(mSubtaskMap.find(pSubtaskId) == mSubtaskMap.end())
        PMTROW(pmFatalErrorException());

    if(!mTask->GetMemSectionRO())
        return false;

    pSubscriptionInfo = mSubtaskMap[pSubtaskId].mInputMemSubscriptions.first[0];

    return true;
}

bool pmSubscriptionManager::GetOutputMemSubscriptionForSubtask(ulong pSubtaskId, pmSubscriptionInfo& pSubscriptionInfo)
{
	FINALIZE_RESOURCE_PTR(dResourceLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mResourceLock, Lock(), Unlock());
	
    if(mSubtaskMap.find(pSubtaskId) == mSubtaskMap.end())
        PMTROW(pmFatalErrorException());

    if(!mTask->GetMemSectionRW())
        return false;

    pSubscriptionInfo = mSubtaskMap[pSubtaskId].mOutputMemSubscriptions.first[0];

    return true;
}

pmStatus pmSubscriptionManager::FetchSubtaskSubscriptions(ulong pSubtaskId)
{
	pmSubscriptionInfo lInputMemSubscription, lOutputMemSubscription;
	subscriptionData lInputMemSubscriptionData, lOutputMemSubscriptionData;

	if(GetInputMemSubscriptionForSubtask(pSubtaskId, lInputMemSubscription))
		FetchSubscription(pSubtaskId, true, lInputMemSubscription, lInputMemSubscriptionData);

	if(GetOutputMemSubscriptionForSubtask(pSubtaskId, lOutputMemSubscription))
		FetchSubscription(pSubtaskId, false, lOutputMemSubscription, lOutputMemSubscriptionData);

	return WaitForSubscriptions(pSubtaskId);
}

pmStatus pmSubscriptionManager::FetchSubscription(ulong pSubtaskId, bool pIsInputMem, pmSubscriptionInfo pSubscriptionInfo, subscriptionData& pData)
{
	pmMemSection* lMemSection = NULL;
		
	if(pIsInputMem)
		lMemSection = mTask->GetMemSectionRO();
	else
		lMemSection = mTask->GetMemSectionRW();

	pData.receiveCommandVector = MEMORY_MANAGER_IMPLEMENTATION_CLASS::GetMemoryManager()->FetchMemoryRegion(lMemSection, mTask->GetPriority(), pSubscriptionInfo.offset, pSubscriptionInfo.length);
	lMemSection->SetRangeOwner(PM_LOCAL_MACHINE, (ulong)(lMemSection->GetMem()), pSubscriptionInfo.offset, pSubscriptionInfo.length);

	return pmSuccess;
}

pmStatus pmSubscriptionManager::WaitForSubscriptions(ulong pSubtaskId)
{
	size_t i, j, lSize, lInnerSize;

	if(mTask->GetMemSectionRO())
	{
        std::vector<subscriptionData> lInputMemVector;
        
        // Auto lock/unlock scope
        {
            FINALIZE_RESOURCE_PTR(dResourceLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mResourceLock, Lock(), Unlock());
            lInputMemVector = mSubtaskMap[pSubtaskId].mInputMemSubscriptions.second[0];
        }

		lSize = lInputMemVector.size();
		for(i=0; i<lSize; ++i)
		{
			std::vector<pmCommunicatorCommandPtr>& lCommandVector = (lInputMemVector[i]).receiveCommandVector;
		
			lInnerSize = lCommandVector.size();
			for(j=0; j<lInnerSize; ++j)
			{
				pmCommunicatorCommandPtr lCommand = lCommandVector[i];
				if(lCommand)
				{
					if(lCommand->WaitForFinish() != pmSuccess)
						PMTHROW(pmMemoryFetchException());
				}
			}
		}
	}

	if(mTask->GetMemSectionRW())
	{
        std::vector<subscriptionData> lOutputMemVector;
        
        // Auto lock/unlock scope
        {
            FINALIZE_RESOURCE_PTR(dResourceLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mResourceLock, Lock(), Unlock());
            lOutputMemVector = mSubtaskMap[pSubtaskId].mOutputMemSubscriptions.second[0];
        }

		lSize = lOutputMemVector.size();
		for(i=0; i<lSize; ++i)
		{
			std::vector<pmCommunicatorCommandPtr>& lCommandVector = (lOutputMemVector[i]).receiveCommandVector;
		
			lInnerSize = lCommandVector.size();
			for(j=0; j<lInnerSize; ++j)
			{
				pmCommunicatorCommandPtr lCommand = lCommandVector[i];
				if(lCommand)
				{
					if(lCommand->WaitForFinish() != pmSuccess)
						PMTHROW(pmMemoryFetchException());
				}
			}
		}
	}

	return pmSuccess;
}
    
pmSubtask::pmSubtask(pmTask* pTask)
{
	pmMemSection* lInputMemSection = pTask->GetMemSectionRO();
	pmMemSection* lOutputMemSection = pTask->GetMemSectionRO();
    
	if(lInputMemSection)
	{
		lInputMemSubscription.offset = 0;
		lInputMemSubscription.length = lInputMemSection->GetLength();
        
        subscriptionData lSubscriptionData;
        std::vector<pmSubscriptionInfo> lVector1;
        std::vector<subscriptionData> lVector2;
        lVector1.push_back(lInputMemSubscription);
        lVector2.push_back(lSubscriptionData);
        mInputMemSubscriptions = SUBSCRIPTION_DATA_TYPE(lVector1, lVector2);
	}
    
	if(lOutputMemSection)
	{
		lOutputMemSubscription.offset = 0;
		lOutputMemSubscription.length = lOutputMemSection->GetLength();

        subscriptionData lSubscriptionData;
        std::vector<pmSubscriptionInfo> lVector1;
        std::vector<subscriptionData> lVector2;
        lVector1.push_back(lOutputMemSubscription);
        lVector2.push_back(lSubscriptionData);
        mOutputMemSubscriptions = SUBSCRIPTION_DATA_TYPE(lVector1, lVector2);
	}
}

}

