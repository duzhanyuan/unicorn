
#ifndef __PM_COMMAND__
#define __PM_COMMAND__

#include "pmInternalDefinitions.h"

namespace pm
{

class pmSignalWait;

/**
 * \brief The command class of PMLIB. Serves as an interface between various PMLIB components like pmControllers.
 * This class defines commands that pmController's, pmThread's, etc. on same/differnt machines/clusters use to communicate.
 * This is the only communication mechanism between pmControllers. The pmCommands are opaque objects
 * and the data interpretation is only known to and handled by command listeners. A pmCommand belongs
 * to a particular category of commands e.g. controller command, thread command, etc.
 * Most command objects are passed among threads. So they should be allocated on heap rather
 * than on local thread stacks. Be cautious to keep alive the memory associated with command objects
 * and the encapsulated data until the execution of a command object finishes.
 * Callers can wait for command to finish by calling WaitForStatus() method.
 * The command executors must set the exit status of command via SetStatus() method. This also wakes
 * up any awaiting threads.
*/

class pmCommand
{
	public:
		pmCommand(ushort pCommandId, void* pCommandData = NULL, ulong pDataLength = 0);

		virtual bool IsValid() = 0;

		virtual ushort GetId() {return mCommandId;}
		virtual void* GetData() {return mCommandData;}
		virtual ulong GetDataLength() {return mDataLength;}
		virtual pmStatus GetStatus() {return mStatus;}

		virtual pmStatus SetData(void* pCommandData, ulong pDataLength);
		virtual pmStatus SetStatus(pmStatus pStatus);

		/**
		 * Block the execution of the calling thread until the status
		 * of the command object becomes available.
		*/
		virtual pmStatus WaitForStatus();

	protected:
		ushort mCommandId;
		void* mCommandData;
		ulong mDataLength;
		pmStatus mStatus;
		pmSignalWait* mSignalWait;
};

class pmCommunicatorCommand : public pmCommand
{
	public:
		typedef enum pmCommunicatorCommands
		{
			NETWORK_COMMUNICATION_COMMAND,
			MAX_COMMUNICATOR_COMMANDS
		} pmCommunicatorCommands;

		pmCommunicatorCommand(ushort pCommandId, void* pCommandData = NULL, ulong pDataLength = 0) : pmCommand(pCommandId, pCommandData, pDataLength) {}
		virtual bool IsValid();

	private:
};

class pmMPICommunicatorCommand : public pmCommunicatorCommand
{
	public:

	private:
		MPI_Request* mRequest;
};

class pmThreadCommand : public pmCommand
{
	public:
		typedef enum pmThreadCommands
		{
			CONTROLLER_COMMAND_WRAPPER,
			MAX_THREAD_COMMANDS
		} pmThreadCommands;

		pmThreadCommand(ushort pCommandId, void* pCommandData = NULL, ulong pDataLength = 0) : pmCommand(pCommandId, pCommandData, pDataLength) {}
		virtual bool IsValid();

	private:
};

} // end namespace pm

#endif