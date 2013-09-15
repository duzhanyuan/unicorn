
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

#include "pmSubtaskSplitter.h"
#include "pmStubManager.h"
#include "pmExecutionStub.h"
#include "pmSubscriptionManager.h"
#include "pmTask.h"

#include <limits>

#ifdef SUPPORT_SPLIT_SUBTASKS

namespace pm
{

using namespace splitter;

pmSubtaskSplitter::pmSubtaskSplitter(pmTask* pTask)
    : mTask(pTask)
    , mSplitFactor(1)
    , mSplitGroups(1)
    , mSplittingType(MAX_DEVICE_TYPES)
{
    pmDeviceType lDeviceType = MAX_DEVICE_TYPES;

    if(mTask->CanSplitCpuSubtasks())
    {
        mSplitFactor = (uint)pmStubManager::GetStubManager()->GetProcessingElementsCPU() / mSplitGroups;
        lDeviceType = CPU;
    }
    else if(mTask->CanSplitGpuSubtasks())
    {
        mSplitFactor = (uint)pmStubManager::GetStubManager()->GetProcessingElementsGPU() / mSplitGroups;
        lDeviceType = GPU_CUDA;
    }

    if(mSplitFactor != 1 && lDeviceType != MAX_DEVICE_TYPES)
    {
        mSplittingType = lDeviceType;

        for(uint i = 0; i < mSplitGroups; ++i)
        {
            pmSplitGroup lSplitGroup(this);
            mSplitGroupVector.push_back(lSplitGroup);
        }
    
        FindConcernedStubs(lDeviceType);
    }
}

pmDeviceType pmSubtaskSplitter::GetSplittingType()
{
    return mSplittingType;
}
    
size_t pmSubtaskSplitter::GetSplitFactor()
{
    return mSplitFactor;
}

bool pmSubtaskSplitter::IsSplitting(pmDeviceType pDeviceType)
{
    return (mSplittingType == pDeviceType);
}
    
bool pmSubtaskSplitter::IsSplitGroupLeader(pmExecutionStub* pStub)
{
    return (mSplitGroupVector[mSplitGroupMap[pStub]].mConcernedStubs[0] == pStub);
}

void pmSubtaskSplitter::FindConcernedStubs(pmDeviceType pDeviceType)
{
    pmStubManager* lStubManager = pmStubManager::GetStubManager();
    
    switch(pDeviceType)
    {
        case CPU:
        {
            size_t lCount = lStubManager->GetProcessingElementsCPU();
            for(size_t i = 0; i < lCount; ++i)
            {
                pmExecutionStub* lStub = lStubManager->GetCpuStub(i);
                size_t lSplitGroupIndex = (size_t)(i / mSplitFactor);
                
                mSplitGroupVector[lSplitGroupIndex].mConcernedStubs.push_back(lStub);
                mSplitGroupMap[lStub] = lSplitGroupIndex;
            }
            
            break;
        }
            
        case GPU_CUDA:
        {
            size_t lCount = lStubManager->GetProcessingElementsGPU();
            for(size_t i = 0; i < lCount; ++i)
            {
                pmExecutionStub* lStub = lStubManager->GetGpuStub(i);
                size_t lSplitGroupIndex = (size_t)(i / mSplitFactor);
                
                mSplitGroupVector[lSplitGroupIndex].mConcernedStubs.push_back(lStub);
                mSplitGroupMap[lStub] = lSplitGroupIndex;
            }

            break;
        }
            
        default:
            PMTHROW(pmFatalErrorException());
    }
}
    
std::auto_ptr<pmSplitSubtask> pmSubtaskSplitter::GetPendingSplit(ulong* pSubtaskId, pmExecutionStub* pSourceStub)
{
    return mSplitGroupVector[mSplitGroupMap[pSourceStub]].GetPendingSplit(pSubtaskId, pSourceStub);
}
    
void pmSubtaskSplitter::FinishedSplitExecution(ulong pSubtaskId, uint pSplitId, pmExecutionStub* pStub, bool pPrematureTermination)
{
    mSplitGroupVector[mSplitGroupMap[pStub]].FinishedSplitExecution(pSubtaskId, pSplitId, pStub, pPrematureTermination);
}

bool pmSubtaskSplitter::Negotiate(pmExecutionStub* pStub, ulong pSubtaskId)
{
    return mSplitGroupVector[mSplitGroupMap[pStub]].Negotiate(pSubtaskId);
}
    
void pmSubtaskSplitter::StubHasProcessedDummyEvent(pmExecutionStub* pStub)
{
    mSplitGroupVector[mSplitGroupMap[pStub]].StubHasProcessedDummyEvent(pStub);
}

void pmSubtaskSplitter::FreezeDummyEvents()
{
    std::vector<pmSplitGroup>::iterator lIter = mSplitGroupVector.begin(), lEndIter = mSplitGroupVector.end();
    
    for(; lIter != lEndIter; ++lIter)
        (*lIter).FreezeDummyEvents();
}

    
/* class pmSplitGroup */
std::auto_ptr<pmSplitSubtask> pmSplitGroup::GetPendingSplit(ulong* pSubtaskId, pmExecutionStub* pSourceStub)
{
    const splitRecord* lSplitRecord = NULL;
    uint lSplitId = std::numeric_limits<uint>::max();

    // Auto lock/unlock scope
    {
        splitRecord* lModifiableSplitRecord = NULL;

        FINALIZE_RESOURCE_PTR(dSplitRecordListLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mSplitRecordListLock, Lock(), Unlock());
     
        if(!mSplitRecordList.empty())
        {
            lModifiableSplitRecord = &mSplitRecordList.back();
            
            if(lModifiableSplitRecord->splitId == lModifiableSplitRecord->splitCount || lModifiableSplitRecord->reassigned)
                lModifiableSplitRecord = NULL;
        }
        
        if(!lModifiableSplitRecord)
        {
            if(!pSubtaskId)
                return std::auto_ptr<pmSplitSubtask>(NULL);
         
            splitRecord lRecord(pSourceStub, *pSubtaskId, mSubtaskSplitter->mSplitFactor);
            mSplitRecordList.push_back(lRecord);
            
            lModifiableSplitRecord = &mSplitRecordList.back();
        }
        
        lSplitId = lModifiableSplitRecord->splitId;

        ++(lModifiableSplitRecord->splitId);
        lModifiableSplitRecord->assignedStubs.push_back(std::make_pair(pSourceStub, false));
        
        lSplitRecord = lModifiableSplitRecord;
    }

    AddDummyEventToRequiredStubs();

    return std::auto_ptr<pmSplitSubtask>(new pmSplitSubtask(mSubtaskSplitter->mTask, lSplitRecord->sourceStub, lSplitRecord->subtaskId, lSplitId, lSplitRecord->splitCount));
}
    
void pmSplitGroup::FinishedSplitExecution(ulong pSubtaskId, uint pSplitId, pmExecutionStub* pStub, bool pPrematureTermination)
{
    bool lCompleted = false;
    splitter::splitRecord lSplitRecord;

    // Auto lock/unlock scope
    {
        FINALIZE_RESOURCE_PTR(dSplitRecordListLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mSplitRecordListLock, Lock(), Unlock());
        
        std::list<splitRecord>::iterator lIter = mSplitRecordList.begin(), lEndIter = mSplitRecordList.end();
        for(; lIter != lEndIter; ++lIter)
        {
            if(lIter->subtaskId == pSubtaskId)
                break;
        }
        
        if(lIter == lEndIter || !lIter->pendingCompletions)
            PMTHROW(pmFatalErrorException());

        --lIter->pendingCompletions;
        
        if(lIter->assignedStubs[pSplitId].first != pStub)
            PMTHROW(pmFatalErrorException());
            
        lIter->assignedStubs[pSplitId].second = true;
        
        if(lIter->pendingCompletions == 0)
        {
            if(pPrematureTermination)
                lIter->reassigned = true;
            
            if(!lIter->reassigned)
            {
                lSplitRecord = *lIter;
                lCompleted = true;
            }
            
            mSplitRecordList.erase(lIter);
        }
    }
    
    if(lCompleted)
    {
        std::vector<std::pair<pmExecutionStub*, bool> >::iterator lInnerIter = lSplitRecord.assignedStubs.begin(), lInnerEndIter = lSplitRecord.assignedStubs.end();
        for(; lInnerIter != lInnerEndIter; ++lInnerIter)
        {
            pmSplitInfo lSplitInfo(lSplitRecord.splitId, lSplitRecord.splitCount);

            (*lInnerIter).first->CommonPostNegotiationOnCPU(mSubtaskSplitter->mTask, pSubtaskId, false, &lSplitInfo);
        }

        pStub->HandleSplitSubtaskExecutionCompletion(mSubtaskSplitter->mTask, lSplitRecord, pmSuccess);
    }
}

void pmSplitGroup::StubHasProcessedDummyEvent(pmExecutionStub* pStub)
{
    FINALIZE_RESOURCE_PTR(dDummyEventLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mDummyEventLock, Lock(), Unlock());
    
    mStubsWithDummyEvent.erase(pStub);
    
    bool lPendingSplits = false;

    // Auto lock/unlock scope
    {
        FINALIZE_RESOURCE_PTR(dSplitRecordListLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mSplitRecordListLock, Lock(), Unlock());
    
        lPendingSplits = !mSplitRecordList.empty();
    }

    if(lPendingSplits && !mDummyEventsFreezed)
        AddDummyEventToStub(pStub);
}
    
void pmSplitGroup::AddDummyEventToRequiredStubs()
{
    FINALIZE_RESOURCE_PTR(dDummyEventLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mDummyEventLock, Lock(), Unlock());
    
    if(mDummyEventsFreezed)
        return;
    
    std::vector<pmExecutionStub*>::iterator lIter = mConcernedStubs.begin(), lEndIter = mConcernedStubs.end();
    for(; lIter != lEndIter; ++lIter)
    {
        if(mStubsWithDummyEvent.find(*lIter) == mStubsWithDummyEvent.end())
            AddDummyEventToStub(*lIter);
    }
}

/* This method must be called with pmSplitGroup's mDummyEventLock acquired */
void pmSplitGroup::AddDummyEventToStub(pmExecutionStub* pStub)
{
    pStub->SplitSubtaskCheckEvent(mSubtaskSplitter->mTask);
    mStubsWithDummyEvent.insert(pStub);
}
    
void pmSplitGroup::FreezeDummyEvents()
{
    std::set<pmExecutionStub*> lPendingStubs;

    // Auto lock/unlock scope
    {
        FINALIZE_RESOURCE_PTR(dDummyEventLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mDummyEventLock, Lock(), Unlock());
        
        mDummyEventsFreezed = true;
        lPendingStubs = mStubsWithDummyEvent;
    }
    
    std::set<pmExecutionStub*>::iterator lIter = lPendingStubs.begin(), lEndIter = lPendingStubs.end();
    for(; lIter != lEndIter; ++lIter)
        (*lIter)->RemoveSplitSubtaskCheckEvent(mSubtaskSplitter->mTask);
}
    
bool pmSplitGroup::Negotiate(ulong pSubtaskId)
{
    bool lRetVal = false;
    std::vector<std::pair<pmExecutionStub*, bool> > lStubVector;

    // Auto lock/unlock scope
    {
        FINALIZE_RESOURCE_PTR(dSplitRecordListLock, RESOURCE_LOCK_IMPLEMENTATION_CLASS, &mSplitRecordListLock, Lock(), Unlock());
        
        std::list<splitRecord>::iterator lIter = mSplitRecordList.begin(), lEndIter = mSplitRecordList.end();
        for(; lIter != lEndIter; ++lIter)
        {
            if(lIter->subtaskId == pSubtaskId)
                break;
        }
        
        if(lIter != lEndIter)
        {
            lStubVector = lIter->assignedStubs;
            lIter->reassigned = true;
            lRetVal = true;
        }
    }
    
    if(lRetVal)
    {
        pmSubtaskRange lRange;
        lRange.task = mSubtaskSplitter->mTask;
        lRange.originalAllottee = NULL;
        lRange.startSubtask = pSubtaskId;
        lRange.endSubtask = pSubtaskId;
        
        std::vector<std::pair<pmExecutionStub*, bool> >::iterator lIter = lStubVector.begin(), lEndIter = lStubVector.end();
        for(; lIter != lEndIter; ++lIter)
        {
            if(!(*lIter).second)
                (*lIter).first->CancelSubtaskRange(lRange);
        }
    }
    
    return lRetVal;
}
    
}

#endif
