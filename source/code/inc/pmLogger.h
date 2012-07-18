
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

#ifndef __PM_LOGGER__
#define __PM_LOGGER__

#include "pmPublicDefinitions.h"
#include "pmDataTypes.h"

namespace pm
{

/**
 * \brief The output/error logger
 */

class pmLogger
{
friend class pmController;
public:
    typedef enum logLevel
    {
        MINIMAL,
        DEATILED,
        DEBUG_INTERNAL	/* Internal use Only; Not for production builds */
    } logLevel;

    typedef enum logType
    {
        INFORMATION,
        WARNING,
        ERROR
    } logType;

    static pmLogger* GetLogger();

    pmStatus SetHostId(uint pHostId);

    pmStatus Log(logLevel pMsgLevel, logType pMsgType, const char* pMsg);

private:
    pmLogger(logLevel pLogLevel);
    virtual ~pmLogger();

    ushort mLogLevel;
    uint mHostId;
    static pmLogger* mLogger;
};

} // end namespace pm

#endif
