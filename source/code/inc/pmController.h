
#ifndef __PM_CONTROLLER__
#define __PM_CONTROLLER__

#include "pmInternalDefinitions.h"

namespace pm
{

/**
 * \brief The top level control daemon on each machine
 * This is a per machine singleton class i.e. exactly one instance of pmController exists per machine.
 * The instance of this class is created when application initializes the library. It is absolutely
 * necessary for the application to initialize the library on all machines participating in the MPI process.
 * The defaulting machines will not be considered by PMLIB for task execution. Once pmController's are created
 * all services are setup and managed by it. The controller's shut down when application finalizes the library.
*/
class pmController
{
	public:
		static pmController* GetController();
		pmStatus DestroyController();

		pmStatus SetLastErrorCode(uint pErrorCode) {mLastErrorCode = pErrorCode; return pmSuccess;}
		uint GetLastErrorCode() {return mLastErrorCode;}

	private:
		pmController() {mController = NULL; mLastErrorCode = 0;}
	
		static pmStatus CreateAndInitializeController();

		static pmController* mController;
		uint mLastErrorCode;
};

} // end namespace pm

#endif