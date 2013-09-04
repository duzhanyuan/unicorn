
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

#ifndef __PM_TASK_MANAGER__
#define __PM_TASK_MANAGER__

#include "pmResourceLock.h"
#include "pmSignalWait.h"
#include "pmCommand.h"

#include <set>
#include <map>

namespace pm
{

class pmTask;
class pmLocalTask;
class pmRemoteTask;
class pmMachine;
class pmProcessingElement;

/**
 * \brief This class manages all tasks executing on this machine. A task may originate locally or remotely.
 * Only one object of this class is created for each machine. This class is thread safe.
 */

class pmTaskManager : public pmBase
{
    typedef std::set<pmLocalTask*> localTasksSetType;
    typedef std::set<pmRemoteTask*> remoteTasksSetType;
    typedef std::map<std::pair<pmMachine*, ulong>, std::vector<std::pair<pmSubtaskRange, pmProcessingElement*> > > enqueuedRemoteSubtasksMapType;

    public:
		static pmTaskManager* GetTaskManager();

		pmStatus SubmitTask(pmLocalTask* pLocalTask);
		pmRemoteTask* CreateRemoteTask(pmCommunicatorCommand::remoteTaskAssignPacked* pRemoteTaskData);

		pmStatus DeleteTask(pmLocalTask* pLocalTask);
		pmStatus DeleteTask(pmRemoteTask* pRemoteTask);

		pmStatus CancelTask(pmLocalTask* pLocalTask);

		uint GetLocalTaskCount();
		uint GetRemoteTaskCount();
    
        pmStatus WaitForAllTasksToFinish();

		uint FindPendingLocalTaskCount();

        bool GetRemoteTaskOrEnqueueSubtasks(pmSubtaskRange& pRange, pmProcessingElement* pTargetDevice, pmMachine* pOriginatingHost, ulong pSequenceNumber);
		pmTask* FindTask(pmMachine* pOriginatingHost, ulong pSequenceNumber);
        pmTask* FindTaskNoThrow(pmMachine* pOriginatingHost, ulong pSequenceNumber);
    
        bool DoesTaskHavePendingSubtasks(pmTask* pTask);
        bool DoesTaskHavePendingSubtasks(pmMachine* pOriginatingHost, ulong pSequenceNumber);
    
	private:
		pmTaskManager();

        pmStatus ScheduleEnqueuedRemoteSubtasksForExecution(pmRemoteTask* pRemoteTask);
        
        pmLocalTask* FindLocalTask_Internal(ulong pSequenceNumber);
        pmRemoteTask* FindRemoteTask_Internal(pmMachine* pOriginatingHost, ulong pSequenceNumber);
		
        pmStatus SubmitTask(pmRemoteTask* pLocalTask);

		static localTasksSetType& GetLocalTasks();		/* Tasks that originated on this machine */
		static remoteTasksSetType& GetRemoteTasks();	/* Tasks that originated on remote machines */
        static enqueuedRemoteSubtasksMapType& GetEnqueuedRemoteSubtasksMap();

		static RESOURCE_LOCK_IMPLEMENTATION_CLASS& GetLocalTaskResourceLock();
		static RESOURCE_LOCK_IMPLEMENTATION_CLASS& GetRemoteTaskResourceLock();

        SIGNAL_WAIT_IMPLEMENTATION_CLASS mTaskFinishSignalWait;
};

} // end namespace pm

#endif
