
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

#ifndef __PM_HEAVY_OPERATIONS__
#define __PM_HEAVY_OPERATIONS__

#include "pmBase.h"
#include "pmThread.h"
#include "pmResourceLock.h"

#include <vector>

namespace pm
{

class pmMemSection;

namespace heavyOperations
{

typedef enum eventIdentifier
{
	PACK_DATA,
    UNPACK_DATA,
    MEM_TRANSFER
} eventIdentifier;
    
typedef struct packEvent
{
    pmCommunicatorCommand::communicatorCommandTags commandTag;
    pmCommunicatorCommand::communicatorDataTypes dataType;
    pmHardware* destination;
    void* data;
    ushort priority;
} packEvent;

typedef struct unpackEvent
{
    char* packedData;
    int packedLength;
} unpackEvent;
    
typedef struct memTransferEvent
{
	pmMemSection* srcMemSection;
    pmCommunicatorCommand::memoryIdentifierStruct destMemIdentifier;
	ulong offset;
	ulong length;
	pmMachine* machine;
    ulong receiverOffset;
	ushort priority;
    bool isForwarded;
} memTransferEvent;

typedef struct heavyOperationsEvent : public pmBasicThreadEvent
{
	eventIdentifier eventId;
	union
	{
		packEvent packDetails;
        unpackEvent unpackDetails;
        memTransferEvent memTransferDetails;
	};
} heavyOperationsEvent;

}
    
class pmHeavyOperationsThread  : public THREADING_IMPLEMENTATION_CLASS<heavyOperations::heavyOperationsEvent>
{
public:
    pmHeavyOperationsThread();
    virtual ~pmHeavyOperationsThread();
    
private:
    virtual pmStatus ThreadSwitchCallback(heavyOperations::heavyOperationsEvent& pEvent);
    pmStatus ProcessEvent(heavyOperations::heavyOperationsEvent& pEvent);
};

class pmHeavyOperationsThreadPool
{
    friend class pmController;
    
public:
    virtual ~pmHeavyOperationsThreadPool();

    void PackAndSendData(pmCommunicatorCommand::communicatorCommandTags pCommandTag, pmCommunicatorCommand::communicatorDataTypes pDataType, pmHardware* pDestination, void* pData, ushort pPriority);
    void UnpackDataEvent(char* pPackedData, int pPackedLength, ushort pPriority);
    void MemTransferEvent(pmMemSection* pSrcMemSection, pmCommunicatorCommand::memoryIdentifierStruct& pDestMemIdentifier, ulong pOffset, ulong pLength, pmMachine* pDestMachine, ulong pReceiverOffset, bool pIsForwarded, ushort pPriority);

    static pmHeavyOperationsThreadPool* GetHeavyOperationsThreadPool();

private:
    pmHeavyOperationsThreadPool(size_t pThreadCount);

    void SubmitToThreadPool(heavyOperations::heavyOperationsEvent& pEvent, ushort pPriority);
    
    static pmHeavyOperationsThreadPool* mHeavyOperationsThreadPool;

    std::vector<pmHeavyOperationsThread*> mThreadVector;
    size_t mCurrentThread;
    
    RESOURCE_LOCK_IMPLEMENTATION_CLASS mResourceLock;
};

}

#endif