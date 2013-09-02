
#include <stdio.h>
#include <time.h>
#include <string.h>

#include "commonAPI.h"
#include "imageFiltering.h"

namespace imageFiltering
{
    
void* gSampleInput;
void* gSerialOutput;
void* gParallelOutput;
char gFilter[MAX_FILTER_DIM][MAX_FILTER_DIM];
    
int gImageWidth, gImageHeight, gImageOffset, gImageBytesPerLine;

void readImageMetaData(char* pImagePath)
{
	FILE* fp = fopen(pImagePath, "rb");
	if(fp == NULL)
	{
		std::cout << "Error in opening image file " << pImagePath << std::endl;
		exit(1);
	}

    bitmapHeader fileHeader;
    
	if(fread((void*)(&fileHeader), sizeof(fileHeader), 1, fp) != 1)
		exit(1);
    
    if(fileHeader.identifier[0] != 'B' || fileHeader.identifier[1] != 'M')
        exit(1);
    
    if(fileHeader.reserved[0] != 0 || fileHeader.reserved[1] != 0)
        exit(1);

#if 0
    if(fileHeader.headersize != 54 || fileHeader.infoSize != 40)
        exit(1);

    if(fileHeader.imageSize + fileHeader.headersize != fileHeader.filesize)
        exit(1);
#endif

    if(fileHeader.bitPlanes != 1 || fileHeader.bitCount != 24 || fileHeader.compression != 0)
        exit(1);
    
    gImageBytesPerLine = (fileHeader.filesize - fileHeader.headersize) / fileHeader.height;
    gImageOffset = fileHeader.headersize;
    
    gImageWidth = fileHeader.width;
    gImageHeight = fileHeader.height;

	fclose(fp);
}

void readImage(char* pImagePath, void* pImageData, bool pInverted)
{
	FILE* fp = fopen(pImagePath, "rb");
	if(fp == NULL)
	{
		std::cout << "Error in opening image file " << pImagePath << std::endl;
		exit(1);
	}

    if(fseek(fp, gImageOffset, SEEK_CUR) != 0)
        exit(1);
    
    char lColor[PIXEL_COUNT];
    unsigned int lSeekOffset = gImageBytesPerLine - (gImageWidth * PIXEL_COUNT);

    for(int i = 0; i < gImageHeight; ++i)
    {
        char* lRow = ((char*)pImageData) + ((pInverted ? i : (gImageHeight - i - 1)) * gImageWidth * PIXEL_COUNT);
        for(int j = 0; j < gImageWidth; ++j)
        {
            if(fread((void*)(&lColor), sizeof(lColor), 1, fp) != 1)
                exit(1);
        
            lRow[PIXEL_COUNT * j] = (pInverted ? lColor[0] : lColor[2]);
            lRow[PIXEL_COUNT * j + 1] = lColor[1];
            lRow[PIXEL_COUNT * j + 2] = (pInverted ? lColor[2] : lColor[0]);
        }

        if(lSeekOffset)
        {
            if(fseek(fp, lSeekOffset, SEEK_CUR) != 0)
                exit(1);
        }
    }
    
	fclose(fp);
}
    
void serialImageFilter(void* pImageData, int pFilterRadius)
{
    char* lImageData = (char*)pImageData;
    char* lSerialOutput = (char*)gSerialOutput;
    
    int lDimMinX, lDimMaxX, lDimMinY, lDimMaxY;

    for(int i = 0; i < gImageHeight; ++i)
    {
        for(int j = 0; j < gImageWidth; ++j)
        {
            lDimMinX = j - pFilterRadius;
            lDimMaxX = j + pFilterRadius;
            lDimMinY = i - pFilterRadius;
            lDimMaxY = i + pFilterRadius;
            
            char lRedVal = 0, lGreenVal = 0, lBlueVal = 0;
            for(int k = lDimMinY; k <= lDimMaxY; ++k)
            {
                for(int l = lDimMinX; l <= lDimMaxX; ++l)
                {
                    int m = ((k < 0) ? 0 : ((k >= gImageHeight) ? (gImageHeight - 1) : k));
                    int n = ((l < 0) ? 0 : ((l >= gImageWidth) ? (gImageWidth - 1) : l));
                    
                    size_t lIndex = (m * gImageWidth + n) * PIXEL_COUNT;
                    lRedVal += lImageData[lIndex] * gFilter[k - lDimMinY][l - lDimMinX];
                    lGreenVal += lImageData[lIndex + 1] * gFilter[k - lDimMinY][l - lDimMinX];
                    lBlueVal += lImageData[lIndex + 2] * gFilter[k - lDimMinY][l - lDimMinX];
                }
            }
            
            size_t lOffset = (i * gImageWidth + j) * PIXEL_COUNT;
            lSerialOutput[lOffset] = lRedVal;
            lSerialOutput[lOffset + 1] = lGreenVal;
            lSerialOutput[lOffset + 2] = lBlueVal;
        }
    }
}

pmStatus imageFilterDataDistribution(pmTaskInfo pTaskInfo, pmRawMemPtr pLazyInputMem, pmRawMemPtr pLazyOutputMem, pmDeviceInfo pDeviceInfo, unsigned long pSubtaskId)
{
	pmSubscriptionInfo lSubscriptionInfo;
	imageFilterTaskConf* lTaskConf = (imageFilterTaskConf*)(pTaskInfo.taskConf);

    unsigned int lTilesPerRow = (lTaskConf->imageWidth/TILE_DIM + (lTaskConf->imageWidth%TILE_DIM ? 1 : 0));
    //unsigned int lTilesPerCol = (lTaskConf->imageHeight/TILE_DIM + (lTaskConf->imageHeight%TILE_DIM ? 1 : 0));

	// Subscribe to one tile of the output matrix
    int lSubscriptionStartCol = (int)((pSubtaskId % lTilesPerRow) * TILE_DIM);
    int lSubscriptionEndCol = lSubscriptionStartCol + TILE_DIM;
    int lSubscriptionStartRow = (int)((pSubtaskId / lTilesPerRow) * TILE_DIM);
    int lSubscriptionEndRow = lSubscriptionStartRow + TILE_DIM;
    
    if(lSubscriptionEndCol > lTaskConf->imageWidth)
        lSubscriptionEndCol = lTaskConf->imageWidth;

    if(lSubscriptionEndRow > lTaskConf->imageHeight)
        lSubscriptionEndRow = lTaskConf->imageHeight;

    size_t lRowSize = lTaskConf->imageWidth * PIXEL_COUNT;
    lSubscriptionInfo.length = (lSubscriptionEndCol - lSubscriptionStartCol) * PIXEL_COUNT;
    for(int i = lSubscriptionStartRow; i < lSubscriptionEndRow; ++i)
    {
        lSubscriptionInfo.offset = (i * lRowSize) + (lSubscriptionStartCol * PIXEL_COUNT);
        pmSubscribeToMemory(pTaskInfo.taskHandle, pDeviceInfo.deviceHandle, pSubtaskId, OUTPUT_MEM_WRITE_SUBSCRIPTION, lSubscriptionInfo);
    }

#ifdef BUILD_CUDA
	// Reserve CUDA Global Mem
	if(pDeviceInfo.deviceType == pm::GPU_CUDA)
    {
        size_t lReservedMem = computeSubtaskReservedMemRequirement(pTaskInfo, pDeviceInfo, pSubtaskId);
		pmReserveCudaGlobalMem(pTaskInfo.taskHandle, pDeviceInfo.deviceHandle, pSubtaskId, lReservedMem);
    }
#endif

	return pmSuccess;
}

pmStatus imageFilter_cpu(pmTaskInfo pTaskInfo, pmDeviceInfo pDeviceInfo, pmSubtaskInfo pSubtaskInfo)
{
	imageFilterTaskConf* lTaskConf = (imageFilterTaskConf*)(pTaskInfo.taskConf);

    char* lInvertedImageData = ((char*)pmGetMappedFile(lTaskConf->imagePath)) + lTaskConf->imageOffset;
    char* lOutput = (char*)(pSubtaskInfo.outputMem);
    
    unsigned int lTilesPerRow = (lTaskConf->imageWidth/TILE_DIM + (lTaskConf->imageWidth%TILE_DIM ? 1 : 0));
    
    int lSubscriptionStartCol = (unsigned int)((pSubtaskInfo.subtaskId % lTilesPerRow) * TILE_DIM);
    int lSubscriptionEndCol = lSubscriptionStartCol + TILE_DIM;
    int lSubscriptionStartRow = (unsigned int)((pSubtaskInfo.subtaskId / lTilesPerRow) * TILE_DIM);
    int lSubscriptionEndRow = lSubscriptionStartRow + TILE_DIM;
    
    if(lSubscriptionEndCol > lTaskConf->imageWidth)
        lSubscriptionEndCol = lTaskConf->imageWidth;

    if(lSubscriptionEndRow > lTaskConf->imageHeight)
        lSubscriptionEndRow = lTaskConf->imageHeight;

    size_t lRowSize = lTaskConf->imageWidth * PIXEL_COUNT;
    size_t lSubtaskOffset = (lSubscriptionStartRow * lRowSize) + (lSubscriptionStartCol * PIXEL_COUNT);

    int lDimMinX, lDimMaxX, lDimMinY, lDimMaxY;

    for(int i = lSubscriptionStartRow; i < lSubscriptionEndRow; ++i)
    {
        for(int j = lSubscriptionStartCol; j < lSubscriptionEndCol; ++j)
        {
            lDimMinX = j - lTaskConf->filterRadius;
            lDimMaxX = j + lTaskConf->filterRadius;
            lDimMinY = i - lTaskConf->filterRadius;
            lDimMaxY = i + lTaskConf->filterRadius;
            
            char lRedVal = 0, lGreenVal = 0, lBlueVal = 0;
            for(int k = lDimMinY; k <= lDimMaxY; ++k)
            {
                for(int l = lDimMinX; l <= lDimMaxX; ++l)
                {
                    int m = ((k < 0) ? 0 : ((k >= lTaskConf->imageHeight) ? (lTaskConf->imageHeight - 1) : k));
                    int n = ((l < 0) ? 0 : ((l >= lTaskConf->imageWidth) ? (lTaskConf->imageWidth - 1) : l));
                    
                    size_t lInvertedIndex = (lTaskConf->imageHeight - 1 - m) * lTaskConf->imageBytesPerLine + n * PIXEL_COUNT;
                    lBlueVal += lInvertedImageData[lInvertedIndex] * lTaskConf->filter[k - lDimMinY][l - lDimMinX];
                    lGreenVal += lInvertedImageData[lInvertedIndex + 1] * lTaskConf->filter[k - lDimMinY][l - lDimMinX];
                    lRedVal += lInvertedImageData[lInvertedIndex + 2] * lTaskConf->filter[k - lDimMinY][l - lDimMinX];
                }
            }
            
            size_t lAbsoluteOffset = (i * lTaskConf->imageWidth + j) * PIXEL_COUNT;
            size_t lOffset = lAbsoluteOffset - lSubtaskOffset;
            lOutput[lOffset] = lRedVal;
            lOutput[lOffset + 1] = lGreenVal;
            lOutput[lOffset + 2] = lBlueVal;
        }
    }
    
	return pmSuccess;
}

#define READ_NON_COMMON_ARGS \
    int lFilterRadius = DEFAULT_FILTER_RADIUS; \
	char* lImagePath = DEFAULT_IMAGE_PATH; \
    FETCH_INT_ARG(lFilterRadius, pCommonArgs, argc, argv); \
	FETCH_STR_ARG(lImagePath, pCommonArgs + 1, argc, argv);

// Returns execution time on success; 0 on error
double DoSerialProcess(int argc, char** argv, int pCommonArgs)
{
	READ_NON_COMMON_ARGS

    void* lImageData = malloc(IMAGE_SIZE);
    readImage(lImagePath, lImageData, false);
    
	double lStartTime = getCurrentTimeInSecs();

	serialImageFilter(lImageData, lFilterRadius);

	double lEndTime = getCurrentTimeInSecs();

    free(lImageData);
	return (lEndTime - lStartTime);
}

// Returns execution time on success; 0 on error
double DoSingleGpuProcess(int argc, char** argv, int pCommonArgs)
{
#ifdef BUILD_CUDA
	READ_NON_COMMON_ARGS

    void* lImageData = malloc(IMAGE_SIZE);
    readImage(lImagePath, lImageData, true);
    
	double lStartTime = getCurrentTimeInSecs();

	if(singleGpuImageFilter(lImageData, gImageWidth, gImageHeight, gFilter, lFilterRadius, gImageBytesPerLine, gParallelOutput) != 0)
        return 0;

	double lEndTime = getCurrentTimeInSecs();

    free(lImageData);
	return (lEndTime - lStartTime);
#else
    return 0;
#endif
}

// Returns execution time on success; 0 on error
double DoParallelProcess(int argc, char** argv, int pCommonArgs, pmCallbackHandle* pCallbackHandle, pmSchedulingPolicy pSchedulingPolicy, bool pFetchBack)
{
	READ_NON_COMMON_ARGS

    if(pmMapFile(lImagePath) != pmSuccess)
        exit(1);

	// Output Mem contains the result image
	// Number of subtasks is equal to the number of rows
    unsigned int lSubtasks = (gImageWidth/TILE_DIM + (gImageWidth%TILE_DIM ? 1 : 0)) * (gImageHeight/TILE_DIM + (gImageHeight%TILE_DIM ? 1 : 0));
	CREATE_TASK(0, IMAGE_SIZE, lSubtasks, pCallbackHandle[0], pSchedulingPolicy)

	imageFilterTaskConf lTaskConf;
    strcpy(lTaskConf.imagePath, lImagePath);
    lTaskConf.imageWidth = gImageWidth;
    lTaskConf.imageHeight = gImageHeight;
    lTaskConf.imageOffset = gImageOffset;
    lTaskConf.imageBytesPerLine = gImageBytesPerLine;
    lTaskConf.filterRadius = lFilterRadius;
    
    int lFilterDim = 2 * lFilterRadius + 1;
    for(int i = 0; i < lFilterDim; ++i)
        for(int j = 0;  j < lFilterDim; ++j)
            lTaskConf.filter[i][j] = gFilter[i][j];

	lTaskDetails.taskConf = (void*)(&lTaskConf);
	lTaskDetails.taskConfLength = sizeof(lTaskConf);

	double lStartTime = getCurrentTimeInSecs();

	SAFE_PM_EXEC( pmSubmitTask(lTaskDetails, &lTaskHandle) );
	
    if(pmWaitForTaskCompletion(lTaskHandle) != pmSuccess)
    {
        FREE_TASK_AND_RESOURCES
        return (double)-1.0;
    }
    
	double lEndTime = getCurrentTimeInSecs();

    if(pFetchBack)
    {
        SAFE_PM_EXEC( pmFetchMemory(lTaskDetails.outputMemHandle) );

        pmRawMemPtr lRawOutputPtr;
        pmGetRawMemPtr(lTaskDetails.outputMemHandle, &lRawOutputPtr);
        memcpy(gParallelOutput, lRawOutputPtr, IMAGE_SIZE);
    }

	FREE_TASK_AND_RESOURCES

    if(pmUnmapFile(lImagePath) != pmSuccess)
        exit(1);

	return (lEndTime - lStartTime);
}

pmCallbacks DoSetDefaultCallbacks()
{
	pmCallbacks lCallbacks;

	lCallbacks.dataDistribution = imageFilterDataDistribution;
	lCallbacks.deviceSelection = NULL;
	lCallbacks.subtask_cpu = imageFilter_cpu;

	#ifdef BUILD_CUDA
	lCallbacks.subtask_gpu_custom = imageFilter_cudaLaunchFunc;
	#endif

	return lCallbacks;
}

// Returns 0 on success; non-zero on failure
int DoInit(int argc, char** argv, int pCommonArgs)
{
	READ_NON_COMMON_ARGS
    
    if(lFilterRadius < MIN_FILTER_RADIUS || lFilterRadius > MAX_FILTER_RADIUS)
    {
        std::cout << "Filter radius must be between " << MIN_FILTER_RADIUS << " and " << MAX_FILTER_RADIUS << std::endl;
        exit(1);
    }

    if(strlen(lImagePath) >= MAX_IMAGE_PATH_LENGTH)
    {
        std::cout << "Image path too long" << std::endl;
        exit(1);
    }
    
    readImageMetaData(lImagePath);

	gSerialOutput = malloc(IMAGE_SIZE);
	gParallelOutput = malloc(IMAGE_SIZE);

    srand((unsigned int)time(NULL));
    
    int lFilterDim = 2 * lFilterRadius + 1;
    for(int i = 0; i < lFilterDim; ++i)
        for(int j = 0; j < lFilterDim; ++j)
            gFilter[i][j] = (((rand() % 2) ? 1 : -1) * rand());
    
	return 0;
}

// Returns 0 on success; non-zero on failure
int DoDestroy()
{
	free(gSerialOutput);
	free(gParallelOutput);

	return 0;
}

// Returns 0 if serial and parallel executions have produced same result; non-zero otherwise
int DoCompare(int argc, char** argv, int pCommonArgs)
{
	READ_NON_COMMON_ARGS
    
    char* lSerialOutput = (char*)gSerialOutput;
    char* lParallelOutput = (char*)gParallelOutput;

    size_t lImageSize = IMAGE_SIZE;
	for(size_t i = 0; i < lImageSize; ++i)
	{
		if(lSerialOutput[i] != lParallelOutput[i])
		{
			std::cout << "Mismatch index " << i << " Serial Value = " << (int)(lSerialOutput[i]) << " Parallel Value = " << (int)(lParallelOutput[i]) << std::endl;
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
    callbackStruct lStruct[1] = { {DoSetDefaultCallbacks, "IMAGEFILTER"} };
    
	commonStart(argc, argv, DoInit, DoSerialProcess, DoSingleGpuProcess, DoParallelProcess, DoCompare, DoDestroy, lStruct, 1);

	commonFinish();

	return 0;
}

}
