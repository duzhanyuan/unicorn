
/*
 * Copyright (c) 2016, Tarun Beri, Sorav Bansal, Subodh Kumar
 * Copyright (c) 2016 Indian Institute of Technology Delhi
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. Any redistribution or
 * modification must retain this copyright notice and appropriately
 * highlight the credits.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * More information about the authors is available at their websites -
 * Prof. Subodh Kumar - http://www.cse.iitd.ernet.in/~subodh/
 * Prof. Sorav Bansal - http://www.cse.iitd.ernet.in/~sbansal/
 * Tarun Beri - http://www.cse.iitd.ernet.in/~tarun
 *
 * All bug reports and enhancement requests can be sent to the following
 * email addresses -
 * onlinetarun@gmail.com
 * sbansal@cse.iitd.ac.in
 * subodh@cse.iitd.ac.in
 */

#include <stdio.h>
#include <iostream>

#include "commonAPI.h"
#include "pageRank.h"

#include <string.h>
#include <math.h>

namespace pageRank
{

unsigned int gTotalWebPages;
unsigned int gMaxOutlinksPerWebPage;
unsigned int gWebPagesPerFile;

PAGE_RANK_DATA_TYPE* gSerialOutput;
PAGE_RANK_DATA_TYPE* gParallelOutput;

void readWebMetaData(char* pBasePath)
{
	char path[1024];

	strcpy(path, pBasePath);
	strcat(path, "/web_dump_metadata");

	FILE* fp = fopen(path, "rb");
	if(fp == NULL)
	{
		std::cout << "Error in opening metadata file " << path << std::endl;
		exit(1);
	}

	if(fread((void*)(&gTotalWebPages), 4, 1, fp) != 1)
		exit(1);

	if(fread((void*)(&gMaxOutlinksPerWebPage), 4, 1, fp) != 1)
		exit(1);

	if(fread((void*)(&gWebPagesPerFile), 4, 1, fp) != 1)
		exit(1);

	std::cout << "Configuration: " << "Pages = " << gTotalWebPages << "; Max outlinks/page = " << gMaxOutlinksPerWebPage << "; Web pages per file = " << gWebPagesPerFile << std::endl;

	fclose(fp);
}

void readWebPagesFile(char* pBasePath, unsigned int pTotalWebPages, unsigned int pMaxOutlinksPerWebPage, unsigned int pWebPagesPerFile, unsigned int pStartPageNum, unsigned int pPageCount, unsigned int* pData)
{
	char filePath[1024];
	char buf[12];
    
    unsigned int lFileNum = 1 + pStartPageNum;
    
	sprintf(buf, "%u", lFileNum);
    strcpy(filePath, pBasePath);
    strcat(filePath, "/web/page_");
    strcat(filePath, buf);

	FILE* fp = fopen(filePath, "rb");
	if(fp == NULL)
	{
		std::cout << "Error in opening page file " << filePath << std::endl;
        exit(1);
	}

	unsigned int lIndex = 0;
    unsigned int lOutlinkCount = 0;
    unsigned int lEndPageNum = pStartPageNum + pPageCount;

    for(unsigned int lPage = (pStartPageNum+1); lPage <= lEndPageNum; ++lPage)
    {
        //std::cout << "Page " << lPage+1 << std::endl;

        if(fread((void*)(&lOutlinkCount), 4, 1, fp) != 1)
            exit(1);

        pData[lIndex] = lOutlinkCount;
        
        //std::cout << "Web Page: " << lPage+1 << " Outlink Count: " << lOutlinkCount << std::endl;

        for(unsigned int j = 0; j < lOutlinkCount; ++j)
        {
            unsigned int lOutlinkPage = pTotalWebPages;
            if(fread((void*)(&lOutlinkPage), 4, 1, fp) != 1)
                exit(1);
        
            if(lOutlinkPage > pTotalWebPages)
            {
                std::cout << "Error: Invalid outlink " << lOutlinkPage << " on page " << pStartPageNum+1 << std::endl;
                exit(1);
            }

            pData[lIndex + 1 + j] = lOutlinkPage;
            //std::cout << lOutlinkPage << " ";
        }

        if(pMaxOutlinksPerWebPage != lOutlinkCount)
        {
            if(fseek(fp, (pMaxOutlinksPerWebPage - lOutlinkCount) * 4, SEEK_CUR) != 0)
                exit(1);
        }

        lIndex += (pMaxOutlinksPerWebPage + 1);

        //std::cout << std::endl;
    }

	fclose(fp);
}
    
void mapAllFiles(char* pBasePath)
{
    unsigned int lTotalFiles = gTotalWebPages / gWebPagesPerFile;
    
    char** filePaths = new char*[lTotalFiles];
    char buf[12];

    for(unsigned int i = 0; i < lTotalFiles; ++i)
    {
        filePaths[i] = new char[1024];
        unsigned int lFileNum = 1 + i * gWebPagesPerFile;
        
        sprintf(buf, "%u", lFileNum);
        strcpy(filePaths[i], pBasePath);
        strcat(filePaths[i], "/web/page_");
        strcat(filePaths[i], buf);
    }

    if(pmMapFiles(filePaths, lTotalFiles) != pmSuccess)
        exit(1);

    for(unsigned int i = 0; i < lTotalFiles; ++i)
        delete[] filePaths[i];
    
    delete[] filePaths;
}

void unMapAllFiles(char* pBasePath)
{
    unsigned int lTotalFiles = gTotalWebPages / gWebPagesPerFile;
    
    char** filePaths = new char*[lTotalFiles];
    char buf[12];

    for(unsigned int i = 0; i < lTotalFiles; ++i)
    {
        filePaths[i] = new char[1024];
        unsigned int lFileNum = 1 + i * gWebPagesPerFile;
        
        sprintf(buf, "%u", lFileNum);
        strcpy(filePaths[i], pBasePath);
        strcat(filePaths[i], "/web/page_");
        strcat(filePaths[i], buf);
    }

    if(pmUnmapFiles(filePaths, lTotalFiles) != pmSuccess)
        exit(1);

    for(unsigned int i = 0; i < lTotalFiles; ++i)
        delete[] filePaths[i];
    
    delete[] filePaths;
}
    
unsigned int getWebPagesPerSubtask(unsigned int pDeviceCount)
{
    unsigned int lWebPagesPerSubtask = 0;

#ifdef ONE_SUBTASK_PER_DEVICE
    unsigned int lFileCount = (gTotalWebPages / gWebPagesPerFile) + ((gTotalWebPages % gWebPagesPerFile) ? 1 : 0);
    unsigned int lFilesPerDevice = (lFileCount / pDeviceCount);
    
    if(lFilesPerDevice == 0)
    {
        std::cout << "Too less files !!!" << std::endl;
        exit(1);
    }
    
    lWebPagesPerSubtask = lFilesPerDevice * gWebPagesPerFile;
#else
    if(gWebPagesPerFile > WEB_PAGES_PER_SUBTASK)
    {
        lWebPagesPerSubtask = gWebPagesPerFile;
    }
    else
    {
        if((WEB_PAGES_PER_SUBTASK % gWebPagesPerFile) == 0)
            lWebPagesPerSubtask = WEB_PAGES_PER_SUBTASK;
        else
            lWebPagesPerSubtask = (WEB_PAGES_PER_SUBTASK / gWebPagesPerFile) * gWebPagesPerFile;
    }
#endif
    
    return lWebPagesPerSubtask;
}
    
void initializePageRankArray(PAGE_RANK_DATA_TYPE* pPageRankArray, PAGE_RANK_DATA_TYPE pVal, unsigned int pCount)
{
	for(unsigned int i = 0; i < pCount; ++i)
		pPageRankArray[i] = pVal;
}

void serialPageRank(unsigned int* pWebDump)
{
	PAGE_RANK_DATA_TYPE* lGlobalPageRankArray = NULL;
	PAGE_RANK_DATA_TYPE* lLocalPageRankArray = NULL;

	for(int i = 0; i < PAGE_RANK_ITERATIONS; ++i)
	{
		if(i != 0)
        {
            delete[] lLocalPageRankArray;
			lLocalPageRankArray = lGlobalPageRankArray;
        }

        if(i == PAGE_RANK_ITERATIONS - 1)
            lGlobalPageRankArray = gSerialOutput;
        else
            lGlobalPageRankArray = new PAGE_RANK_DATA_TYPE[gTotalWebPages];

        memset(lGlobalPageRankArray, 0, gTotalWebPages * sizeof(PAGE_RANK_DATA_TYPE));
        //initializePageRankArray(lGlobalPageRankArray, 1.0 - DAMPENING_FACTOR, gTotalWebPages);

		unsigned int index = 0;
		for(unsigned int j = 0; j < gTotalWebPages; ++j)
		{
			unsigned int lOutlinks = pWebDump[index++];
            PAGE_RANK_DATA_TYPE lIncr = (PAGE_RANK_DATA_TYPE)(DAMPENING_FACTOR * ((i == 0) ? INITIAL_PAGE_RANK : lLocalPageRankArray[j])/(float)lOutlinks);
        
			for(unsigned int k = 0; k < lOutlinks; ++k)
			{
				unsigned int lRefLink = pWebDump[index + k];

				lGlobalPageRankArray[lRefLink - 1] += lIncr;
			}
        
			index += gMaxOutlinksPerWebPage;
		}
	}

	delete[] lLocalPageRankArray;
}

pmStatus pageRankDataDistribution(pmTaskInfo pTaskInfo, pmDeviceInfo pDeviceInfo, pmSubtaskInfo pSubtaskInfo)
{
	pageRankTaskConf* lTaskConf = (pageRankTaskConf*)(pTaskInfo.taskConf);
    
    bool lPartialLastSubtask = (lTaskConf->totalWebPages < ((pSubtaskInfo.subtaskId + 1) * lTaskConf->webPagesPerSubtask));
    unsigned int lStartPage = (unsigned int)(pSubtaskInfo.subtaskId * lTaskConf->webPagesPerSubtask);
    unsigned int lWebPages = (unsigned int)(lPartialLastSubtask ? (lTaskConf->totalWebPages - (pSubtaskInfo.subtaskId * lTaskConf->webPagesPerSubtask)) : lTaskConf->webPagesPerSubtask);

	// Subscribe to an input memory partition (no data is required for first iteration)
    if(lTaskConf->iteration != 0)
    {
        pmSubscriptionInfo lSubscriptionInfo(lStartPage * sizeof(PAGE_RANK_DATA_TYPE), sizeof(PAGE_RANK_DATA_TYPE) * lWebPages);

        pmSubscribeToMemory(pTaskInfo.taskHandle, pDeviceInfo.deviceHandle, pSubtaskInfo.subtaskId, pSubtaskInfo.splitInfo, INPUT_MEM_INDEX, READ_SUBSCRIPTION, lSubscriptionInfo);
    }

	// Subscribe to entire output matrix (default behaviour)
    pmSubscribeToMemory(pTaskInfo.taskHandle, pDeviceInfo.deviceHandle, pSubtaskInfo.subtaskId, pSubtaskInfo.splitInfo, OUTPUT_MEM_INDEX, WRITE_SUBSCRIPTION, pmSubscriptionInfo(0, lTaskConf->totalWebPages * sizeof(PAGE_RANK_DATA_TYPE)));

#ifdef BUILD_CUDA
	// Set CUDA Launch Configuration
	if(pDeviceInfo.deviceType == pm::GPU_CUDA)
    {
        size_t lReservedMem = lWebPages * (lTaskConf->maxOutlinksPerWebPage + 1) * sizeof(PAGE_RANK_DATA_TYPE);
        pmReserveCudaGlobalMem(pTaskInfo.taskHandle, pDeviceInfo.deviceHandle, pSubtaskInfo.subtaskId, pSubtaskInfo.splitInfo, lReservedMem);
    }
#endif

	return pmSuccess;
}
    
void** LoadMappedFiles(pageRankTaskConf* pTaskConf, ulong pSubtaskId)
{
    unsigned int lWebPages = (unsigned int)((pTaskConf->totalWebPages < ((pSubtaskId + 1) * pTaskConf->webPagesPerSubtask)) ? (pTaskConf->totalWebPages - (pSubtaskId * pTaskConf->webPagesPerSubtask)) : pTaskConf->webPagesPerSubtask);
    
    unsigned int lWebFiles = ((lWebPages / pTaskConf->webPagesPerFile) + ((lWebPages % pTaskConf->webPagesPerFile) ? 1 : 0));
    unsigned int lFirstWebFile = (unsigned int)pSubtaskId * lWebFiles;
    unsigned int lLastWebFile = lFirstWebFile + lWebFiles;
    
    void** lWebFilePtrs = new void*[lWebFiles];
    for(unsigned int fileIndex = lFirstWebFile; fileIndex < lLastWebFile; ++fileIndex)
    {
        char filePath[1024];
        char buf[12];
        
        unsigned int lFileNum = 1 + fileIndex * pTaskConf->webPagesPerFile;
        
        sprintf(buf, "%u", lFileNum);
        strcpy(filePath, pTaskConf->basePath);
        strcat(filePath, "/web/page_");
        strcat(filePath, buf);
        
        lWebFilePtrs[fileIndex - lFirstWebFile] = pmGetMappedFile(filePath);
    }
    
    return lWebFilePtrs;
}

pmStatus pageRank_cpu(pmTaskInfo pTaskInfo, pmDeviceInfo pDeviceInfo, pmSubtaskInfo pSubtaskInfo)
{
	pageRankTaskConf* lTaskConf = (pageRankTaskConf*)(pTaskInfo.taskConf);

	PAGE_RANK_DATA_TYPE* lLocalArray = ((lTaskConf->iteration == 0) ? NULL : (PAGE_RANK_DATA_TYPE*)pSubtaskInfo.memInfo[INPUT_MEM_INDEX].ptr);
	PAGE_RANK_DATA_TYPE* lGlobalArray = (PAGE_RANK_DATA_TYPE*)pSubtaskInfo.memInfo[OUTPUT_MEM_INDEX].ptr;
    
    memset(lGlobalArray, 0, pSubtaskInfo.memInfo[OUTPUT_MEM_INDEX].length);

    ulong lSubtaskId = pSubtaskInfo.subtaskId;
    void** lWebFilePtrs = LoadMappedFiles(lTaskConf, lSubtaskId);

    unsigned int lWebPages = (unsigned int)((lTaskConf->totalWebPages < ((lSubtaskId + 1) * lTaskConf->webPagesPerSubtask)) ? (lTaskConf->totalWebPages - (lSubtaskId * lTaskConf->webPagesPerSubtask)) : lTaskConf->webPagesPerSubtask);
    unsigned int lWebFiles = ((lWebPages / lTaskConf->webPagesPerFile) + ((lWebPages % lTaskConf->webPagesPerFile) ? 1 : 0));
    unsigned int lFirstWebFile = (unsigned int)pSubtaskInfo.subtaskId * lWebFiles;

    unsigned int lTotalFiles = (lTaskConf->totalWebPages / lTaskConf->webPagesPerFile) + ((lTaskConf->totalWebPages % lTaskConf->webPagesPerFile) ? 1 : 0);
    for(unsigned int i = 0; i < lWebFiles; ++i)
    {
        unsigned int* lMappedFile = (unsigned int*)(lWebFilePtrs[i]);

        unsigned int index = 0;
        unsigned int lPagesInFile = lTaskConf->webPagesPerFile;
        if(i + lFirstWebFile == lTotalFiles - 1)
            lPagesInFile = lTaskConf->totalWebPages - (i + lFirstWebFile) * lTaskConf->webPagesPerFile;

        for(unsigned int j = 0; j < lPagesInFile; ++j)
        {
            unsigned int lPageNum = i * lTaskConf->webPagesPerFile + j;

            unsigned int lOutlinks = lMappedFile[index++];
            PAGE_RANK_DATA_TYPE lIncr = (PAGE_RANK_DATA_TYPE)(DAMPENING_FACTOR * ((lTaskConf->iteration == 0) ? lTaskConf->initialPageRank : lLocalArray[lPageNum])/(float)lOutlinks);

            for(unsigned int k = 0; k < lOutlinks; ++k)
                lGlobalArray[lMappedFile[index + k] - 1] += lIncr;
        
            index += lTaskConf->maxOutlinksPerWebPage;
        }
    }

    delete[] lWebFilePtrs;
	return pmSuccess;
}
    
#define READ_NON_COMMON_ARGS \
	char* lBasePath = DEFAULT_BASE_PATH; \
	FETCH_STR_ARG(lBasePath, pCommonArgs, argc, argv);

// Returns execution time on success; 0 on error
double DoSerialProcess(int argc, char** argv, int pCommonArgs)
{
	READ_NON_COMMON_ARGS

	double lStartTime = getCurrentTimeInSecs();

	unsigned int* lWebDump = new unsigned int[gTotalWebPages * (gMaxOutlinksPerWebPage + 1)];

    unsigned int lTotalFiles = (gTotalWebPages / gWebPagesPerFile);
    unsigned int i = 0;
    for(; i < lTotalFiles; ++i)
        readWebPagesFile(lBasePath, gTotalWebPages, gMaxOutlinksPerWebPage, gWebPagesPerFile, i*gWebPagesPerFile, gWebPagesPerFile, lWebDump + (i * gWebPagesPerFile * (gMaxOutlinksPerWebPage + 1)));
    
    if((gTotalWebPages % gWebPagesPerFile) != 0)
        readWebPagesFile(lBasePath, gTotalWebPages, gMaxOutlinksPerWebPage, gWebPagesPerFile, i*gWebPagesPerFile, gTotalWebPages - i*gWebPagesPerFile, lWebDump + (i * gWebPagesPerFile * (gMaxOutlinksPerWebPage + 1)));

	serialPageRank(lWebDump);

    delete[] lWebDump;

	double lEndTime = getCurrentTimeInSecs();

	return (lEndTime - lStartTime);
}
    
// Returns execution time on success; 0 on error
double DoSingleGpuProcess(int argc, char** argv, int pCommonArgs)
{
#ifdef BUILD_CUDA
	READ_NON_COMMON_ARGS

	pageRankTaskConf lTaskConf;
	lTaskConf.totalWebPages = gTotalWebPages;
    lTaskConf.maxOutlinksPerWebPage = gMaxOutlinksPerWebPage;
    lTaskConf.webPagesPerFile = gWebPagesPerFile;
    lTaskConf.webPagesPerSubtask = 0;
    lTaskConf.initialPageRank = INITIAL_PAGE_RANK;
    strcpy(lTaskConf.basePath, lBasePath);

	unsigned int* lWebDump = new unsigned int[gTotalWebPages * (gMaxOutlinksPerWebPage + 1)];
    
    unsigned int lTotalFiles = (gTotalWebPages / gWebPagesPerFile);
    unsigned int i = 0;
    for(; i < lTotalFiles; ++i)
        readWebPagesFile(lBasePath, gTotalWebPages, gMaxOutlinksPerWebPage, gWebPagesPerFile, i*gWebPagesPerFile, gWebPagesPerFile, lWebDump + (i * gWebPagesPerFile * (gMaxOutlinksPerWebPage + 1)));
    
    if((gTotalWebPages % gWebPagesPerFile) != 0)
        readWebPagesFile(lBasePath, gTotalWebPages, gMaxOutlinksPerWebPage, gWebPagesPerFile, i*gWebPagesPerFile, gTotalWebPages - i*gWebPagesPerFile, lWebDump + (i * gWebPagesPerFile * (gMaxOutlinksPerWebPage + 1)));
    
	double lStartTime = getCurrentTimeInSecs();

	if(singleGpuPageRank(lTaskConf, lWebDump, gParallelOutput) != 0)
        return 0;

	double lEndTime = getCurrentTimeInSecs();

    delete[] lWebDump;

	return (lEndTime - lStartTime);
#else
    return 0;
#endif
}

bool ParallelPageRankIteration(pmMemHandle pInputMemHandle, pmMemHandle pOutputMemHandle, pageRankTaskConf* pTaskConf, pmCallbackHandle pCallbackHandle, pmSchedulingPolicy pSchedulingPolicy)
{
    pTaskConf->webPagesPerSubtask = getWebPagesPerSubtask(pmGetHostCount() * 4);    // Assuming 5 devices per host
    unsigned long lSubtasks = (gTotalWebPages / pTaskConf->webPagesPerSubtask) + ((gTotalWebPages % pTaskConf->webPagesPerSubtask) ? 1 : 0);

    pmTaskMem lTaskMem[MAX_MEM_INDICES];

    CREATE_TASK(lSubtasks, pCallbackHandle, pSchedulingPolicy)

    if(pInputMemHandle)
        lTaskMem[INPUT_MEM_INDEX] = {pInputMemHandle,  READ_ONLY, SUBSCRIPTION_NATURAL};
    
    lTaskMem[OUTPUT_MEM_INDEX] = {pOutputMemHandle, WRITE_ONLY, SUBSCRIPTION_NATURAL};

    lTaskDetails.taskMemCount = (pInputMemHandle ? MAX_MEM_INDICES : 1);
    lTaskDetails.taskMem = (pmTaskMem*)(lTaskMem);
    
	lTaskDetails.taskConf = (void*)(pTaskConf);
	lTaskDetails.taskConfLength = sizeof(pageRankTaskConf);

	SAFE_PM_EXEC( pmSubmitTask(lTaskDetails, &lTaskHandle) );
	
    if(pmWaitForTaskCompletion(lTaskHandle) != pmSuccess)
    {
        FREE_TASK_AND_RESOURCES
        return false;
    }

    pmReleaseTask(lTaskHandle);

    return true;
}

// Returns execution time on success; 0 on error
double DoParallelProcess(int argc, char** argv, int pCommonArgs, pmCallbackHandle* pCallbackHandle, pmSchedulingPolicy pSchedulingPolicy, bool pFetchBack)
{
	READ_NON_COMMON_ARGS
    
	double lStartTime = getCurrentTimeInSecs();

    mapAllFiles(lBasePath);

	// Input Mem contains page rank of each page before the iteration
	// Output Mem contains the page rank of each page after the iteration
	// Number of subtasks is equal to the number of rows
	size_t lMemSize = gTotalWebPages * sizeof(PAGE_RANK_DATA_TYPE);

    pmMemHandle lInputMemHandle = NULL, lOutputMemHandle = NULL;

    CREATE_MEM(lMemSize, lOutputMemHandle);
    
    if(PAGE_RANK_ITERATIONS > 1)
        CREATE_MEM(lMemSize, lInputMemHandle);

    pageRankTaskConf lTaskConf;
	lTaskConf.totalWebPages = gTotalWebPages;
    lTaskConf.maxOutlinksPerWebPage = gMaxOutlinksPerWebPage;
    lTaskConf.webPagesPerFile = gWebPagesPerFile;
    lTaskConf.webPagesPerSubtask = 0;
    lTaskConf.initialPageRank = INITIAL_PAGE_RANK;
    strcpy(lTaskConf.basePath, lBasePath);

    for(int i = 0; i < PAGE_RANK_ITERATIONS; ++i)
    {
		if(i != 0)
            std::swap(lInputMemHandle, lOutputMemHandle);
    
        lTaskConf.iteration = i;
        if(!ParallelPageRankIteration(((i == 0) ? NULL : lInputMemHandle), lOutputMemHandle, &lTaskConf, pCallbackHandle[0], pSchedulingPolicy))
			return (double)-1.0;
    }
    
    unMapAllFiles(lBasePath);

	double lEndTime = getCurrentTimeInSecs();

    if(pFetchBack)
    {
        SAFE_PM_EXEC( pmFetchMemory(lOutputMemHandle) );

        pmRawMemPtr lRawOutputPtr;
        pmGetRawMemPtr(lOutputMemHandle, &lRawOutputPtr);
        memcpy(gParallelOutput, lRawOutputPtr, lMemSize);
    }

    if(lInputMemHandle)
        pmReleaseMemory(lInputMemHandle);

	pmReleaseMemory(lOutputMemHandle);

	return (lEndTime - lStartTime);
}

pmCallbacks DoSetDefaultCallbacks()
{
	pmCallbacks lCallbacks;

	lCallbacks.dataDistribution = pageRankDataDistribution;
	lCallbacks.deviceSelection = NULL;
	lCallbacks.subtask_cpu = pageRank_cpu;

#ifdef BUILD_CUDA
	lCallbacks.subtask_gpu_custom = pageRank_cudaLaunchFunc;
#endif

#if PAGE_RANK_DATA_TYPE == float
    lCallbacks.dataReduction = pmGetSubtaskReductionCallbackImpl(REDUCE_ADD, REDUCE_FLOATS);
#elif PAGE_RANK_DATA_TYPE == int
    lCallbacks.dataReduction = pmGetSubtaskReductionCallbackImpl(REDUCE_ADD, REDUCE_INTS);
#else
#error "Unsupported data type"
#endif

	return lCallbacks;
}

// Returns 0 on success; non-zero on failure
int DoInit(int argc, char** argv, int pCommonArgs)
{
	READ_NON_COMMON_ARGS
    
    if(strlen(lBasePath) >= MAX_BASE_PATH_LENGTH)
    {
        std::cout << "Base path too long" << std::endl;
        exit(1);
    }

    readWebMetaData(lBasePath);

	gSerialOutput = new PAGE_RANK_DATA_TYPE[gTotalWebPages];
	gParallelOutput = new PAGE_RANK_DATA_TYPE[gTotalWebPages];
    
	return 0;
}

// Returns 0 on success; non-zero on failure
int DoDestroy()
{
	delete[] gSerialOutput;
	delete[] gParallelOutput;

	return 0;
}

// Returns 0 if serial and parallel executions have produced same result; non-zero otherwise
int DoCompare(int argc, char** argv, int pCommonArgs)
{
	for(unsigned int i = 0; i < gTotalWebPages; ++i)
	{
		if((int)(fabs(gSerialOutput[i] - gParallelOutput[i])) > 0)
		{
			std::cout << "Mismatch index " << i << " Serial Value = " << gSerialOutput[i] << " Parallel Value = " << gParallelOutput[i] << std::endl;
			return 1;
		}
	}

	return 0;
}

/**	Non-common args
 *	1. Matrix Dimension
 */
int main(int argc, char** argv)
{
    callbackStruct lStruct[1] = { {DoSetDefaultCallbacks, "PAGERANK"} };

	commonStart(argc, argv, DoInit, DoSerialProcess, DoSingleGpuProcess, DoParallelProcess, DoCompare, DoDestroy, lStruct, 1);

	commonFinish();

	return 0;
}

}
