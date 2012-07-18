
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

#ifndef __PM_SUBSCRIPTION_MANAGER__
#define __PM_SUBSCRIPTION_MANAGER__

#include "pmBase.h"
#include "pmResourceLock.h"
#include "pmCommand.h"

#include <map>

namespace pm
{

class pmTask;

namespace subscription
{
	typedef struct subscriptionData
	{
		std::vector<pmCommunicatorCommandPtr> receiveCommandVector;
	} subscriptionData;

    typedef std::map<size_t, std::pair<size_t, subscriptionData> > subscriptionRecordType;

	typedef struct pmSubtask
	{
		pmCudaLaunchConf mCudaLaunchConf;
        finalize_ptr_array<char> mScratchBuffer;

        pmSubscriptionInfo mConsolidatedInputMemSubscription;   // The contiguous range enclosing all ranges in mInputMemSubscriptions
        pmSubscriptionInfo mConsolidatedOutputMemSubscription;   // The contiguous range enclosing all ranges in mOutputMemSubscriptions
        
		subscriptionRecordType mInputMemSubscriptions;
		subscriptionRecordType mOutputMemSubscriptions;

		pmStatus Initialize(pmTask* pTask);
	} pmSubtask;
}

class pmSubscriptionManager : public pmBase
{
	public:
		pmSubscriptionManager(pmTask* pTask);
		virtual ~pmSubscriptionManager();

		pmStatus InitializeSubtaskDefaults(ulong pSubtaskId);
		pmStatus RegisterSubscription(ulong pSubtaskId, bool pIsInputMem, pmSubscriptionInfo pSubscriptionInfo);
		pmStatus FetchSubtaskSubscriptions(ulong pSubtaskId, pmDeviceTypes pDeviceType);
		pmStatus SetCudaLaunchConf(ulong pSubtaskId, pmCudaLaunchConf& pCudaLaunchConf);
		pmCudaLaunchConf& GetCudaLaunchConf(ulong pSubtaskId);
        void* GetScratchBuffer(ulong pSubtaskId, size_t pBufferSize);

		bool GetInputMemSubscriptionForSubtask(ulong pSubtaskId, pmSubscriptionInfo& pSubscriptionInfo);
		bool GetOutputMemSubscriptionForSubtask(ulong pSubtaskId, pmSubscriptionInfo& pSubscriptionInfo);
    
        bool GetNonConsolidatedInputMemSubscriptionsForSubtask(ulong pSubtaskId, subscription::subscriptionRecordType::const_iterator& pBegin, subscription::subscriptionRecordType::const_iterator& pEnd);
        bool GetNonConsolidatedOutputMemSubscriptionsForSubtask(ulong pSubtaskId, subscription::subscriptionRecordType::const_iterator& pBegin, subscription::subscriptionRecordType::const_iterator& pEnd);

        bool SubtasksHaveMatchingSubscriptions(ulong pSubtaskId1, ulong pSubtaskId2, bool pIsInputMem);

	private:
		pmStatus WaitForSubscriptions(ulong pSubtaskId);
		pmStatus FetchSubscription(ulong pSubtaskId, bool pIsInputMem, pmDeviceTypes pDeviceType, pmSubscriptionInfo pSubscriptionInfo, subscription::subscriptionData& pData);

		RESOURCE_LOCK_IMPLEMENTATION_CLASS mResourceLock;
		std::map<ulong, subscription::pmSubtask> mSubtaskMap;     // subtaskId to pmSubtask map

		pmTask* mTask;
};

bool operator==(pmSubscriptionInfo& pSubscription1, pmSubscriptionInfo& pSubscription2);
bool operator!=(pmSubscriptionInfo& pSubscription1, pmSubscriptionInfo& pSubscription2);

} // end namespace pm

#endif
