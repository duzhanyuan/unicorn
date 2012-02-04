
#include "pmLogger.h"
#include "pmNetwork.h"

#define LOG_LEVEL pmLogger::MINIMAL

namespace pm
{

pmLogger* pmLogger::mLogger = NULL;

pmLogger* pmLogger::GetLogger()
{
	if(!mLogger)
		mLogger = new pmLogger(LOG_LEVEL);

	return mLogger;
}

pmStatus pmLogger::DestroyLogger()
{
	delete mLogger;
	mLogger = NULL;

	return pmSuccess;
}

pmLogger::pmLogger(logLevel pLogLevel)
{
	mHostId = (uint)-1;	// Unknown initially
	mLogLevel = pLogLevel;
}

pmLogger::~pmLogger()
{
}

pmStatus pmLogger::SetHostId(uint pHostId)
{
	mHostId = pHostId;

	return pmSuccess;
}

pmStatus pmLogger::Log(logLevel pMsgLevel, logType pMsgType, const char* pMsg)
{
	if(pMsgLevel <= mLogLevel)
	{
		if(pMsgType == INFORMATION)
			fprintf(stdout, "PMLIB [Host %d] %s\n", mHostId, pMsg);
		else
			fprintf(stderr, "PMLIB [Host %d] %s\n", mHostId, pMsg);
	}

	return pmSuccess;
}

} // end namespace pm


