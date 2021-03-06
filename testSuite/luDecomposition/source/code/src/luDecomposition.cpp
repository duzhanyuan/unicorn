
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

#include <time.h>
#include <string.h>
#include <math.h>

#include <memory>

#include "commonAPI.h"
#include "luDecomposition.h"

// Inplace LU Decomposition of a Square Matrix stored in Column Major Format

namespace luDecomposition
{

#if defined(MATRIX_DATA_TYPE_FLOAT)
#define CBLAS_SCAL cblas_sscal
#define CBLAS_GER cblas_sger
#define CBLAS_TRSM cblas_strsm
#define CBLAS_GEMM cblas_sgemm
#define CBLAS_TRMM cblas_strmm
#define CBLAS_AXPY cblas_saxpy
#elif defined(MATRIX_DATA_TYPE_DOUBLE)
#define CBLAS_SCAL cblas_dscal
#define CBLAS_GER cblas_dger
#define CBLAS_TRSM cblas_dtrsm
#define CBLAS_GEMM cblas_dgemm
#define CBLAS_TRMM cblas_dtrmm
#define CBLAS_AXPY cblas_daxpy
#endif

MATRIX_DATA_TYPE* gSerialOutput;
MATRIX_DATA_TYPE* gParallelOutput;

std::unique_ptr<MATRIX_DATA_TYPE> reproduceNonDecomposedMatrix(MATRIX_DATA_TYPE* pMatrix, size_t pDim)
{
    size_t lElems = pDim * pDim;

    std::unique_ptr<MATRIX_DATA_TYPE> lLower(new MATRIX_DATA_TYPE[lElems]);
    std::unique_ptr<MATRIX_DATA_TYPE> lUpper(new MATRIX_DATA_TYPE[lElems]);
    
	memset(lLower.get(), 0, lElems * sizeof(MATRIX_DATA_TYPE));
	memset(lUpper.get(), 0, lElems * sizeof(MATRIX_DATA_TYPE));

    for(size_t i = 0; i < pDim; ++i)
    {
        for(size_t j = 0; j < i; ++j)
            (lLower.get())[i * pDim + j] = pMatrix[i * pDim + j];

        for(size_t j = i + 1; j < pDim; ++j)
            (lUpper.get())[i * pDim + j] = pMatrix[i * pDim + j];
        
        (lLower.get())[i * pDim + i] = 1.0;
        (lUpper.get())[i * pDim + i] = pMatrix[i * pDim + i];
    }
    
    CBLAS_TRMM(CblasColMajor, CblasRight, CblasUpper, CblasNoTrans, CblasUnit, (int)pDim, (int)pDim, 1.0f, lUpper.get(), (int)pDim, lLower.get(), (int)pDim);

    return lLower;
}
    
float findFrobeniusNormal(MATRIX_DATA_TYPE* pMatrix, size_t pDim)
{
    MATRIX_DATA_TYPE lSum = 0.0f;

    for(size_t i = 0; i < pDim; ++i)
        for(size_t j = 0; j < pDim; ++j)
            lSum += fabsf(pMatrix[i * pDim + j]) * fabsf(pMatrix[i * pDim + j]);
    
	return sqrtf(lSum);
}
    
bool isSingularMatrix(MATRIX_DATA_TYPE* pMatrix, size_t pDim)
{
    for(size_t i = 0; i < pDim; ++i)
    {
        if(pMatrix[i * pDim + i] == 0.0)
            return true;
    }
    
    return false;
}
    
void serialLUDecomposition(MATRIX_DATA_TYPE* pMatrix, size_t pDim, size_t pColStepElems)
{
    for(size_t i = 0; i < pDim - 1; ++i)
    {
        MATRIX_DATA_TYPE lDiag = pMatrix[i + i * pColStepElems];
		
        CBLAS_SCAL((int)(pDim - i - 1), (1.0/lDiag), &pMatrix[i + (i + 1) * pColStepElems], (int)pColStepElems);
        CBLAS_GER(CblasColMajor, (int)(pDim - i - 1), (int)(pDim - i - 1), -1.0f, &pMatrix[(i + 1) + i * pColStepElems], 1, &pMatrix[i + (i + 1) * pColStepElems], (int)pColStepElems, &pMatrix[(i + 1) + (i + 1) * pColStepElems], (int)pColStepElems);
    }
}

pmStatus luDecompositionDataDistribution(pmTaskInfo pTaskInfo, pmDeviceInfo pDeviceInfo, pmSubtaskInfo pSubtaskInfo)
{
	luTaskConf* lTaskConf = (luTaskConf*)(pTaskInfo.taskConf);
    
    // Subscribe to one block of matrix
    SUBSCRIBE_BLOCK(pTaskInfo.taskId, pTaskInfo.taskId, lTaskConf->matrixDim, pSubtaskInfo.subtaskId, pSubtaskInfo.splitInfo, READ_WRITE_SUBSCRIPTION);

#ifdef BUILD_CUDA
	// Reserve CUDA Global Mem
	if(pDeviceInfo.deviceType == pm::GPU_CUDA)
    {
        size_t lDiagonalElemSize = sizeof(MATRIX_DATA_TYPE);
		pmReserveCudaGlobalMem(pTaskInfo.taskHandle, pDeviceInfo.deviceHandle, pSubtaskInfo.subtaskId, pSubtaskInfo.splitInfo, lDiagonalElemSize);
    }
#endif

	return pmSuccess;
}
    
pmStatus luDecomposition_cpu(pmTaskInfo pTaskInfo, pmDeviceInfo pDeviceInfo, pmSubtaskInfo pSubtaskInfo)
{
	luTaskConf* lTaskConf = (luTaskConf*)(pTaskInfo.taskConf);
    MATRIX_DATA_TYPE* lOutputMem = ((MATRIX_DATA_TYPE*)pSubtaskInfo.memInfo[OUTPUT_MEM_INDEX].ptr);

    size_t lColStepSize = ((pSubtaskInfo.memInfo[OUTPUT_MEM_INDEX].visibilityType == SUBSCRIPTION_NATURAL) ? lTaskConf->matrixDim : BLOCK_DIM);
    serialLUDecomposition(lOutputMem, BLOCK_DIM, lColStepSize);

	return pmSuccess;
}
    
bool GetSubtaskSplit(size_t* pStartDim, size_t* pEndDim, pmSplitInfo pSplitInfo)
{
    *pStartDim = 0;
    *pEndDim = BLOCK_DIM;
    
    // If it is a split subtask, then subscribe to a smaller number of cols within the col major block
    if(pSplitInfo.splitCount)
    {
        size_t lSplitCount = pSplitInfo.splitCount;
        size_t lDims = (*pEndDim - *pStartDim);

        if(lSplitCount > lDims)
            lSplitCount = lDims;
        
        // No subscription required
        if((size_t)pSplitInfo.splitId >= lSplitCount)
            return false;
        
        size_t lDimFactor = lDims / lSplitCount;

        *pStartDim = pSplitInfo.splitId * lDimFactor;
        if((size_t)pSplitInfo.splitId != lSplitCount - 1)
            *pEndDim = (*pStartDim + lDimFactor);
    }
    
    return true;
}

pmStatus horizVertCompDataDistribution(pmTaskInfo pTaskInfo, pmDeviceInfo pDeviceInfo, pmSubtaskInfo pSubtaskInfo)
{
	luTaskConf* lTaskConf = (luTaskConf*)(pTaskInfo.taskConf);
    
    size_t lStartDim, lEndDim;
    if(!GetSubtaskSplit(&lStartDim, &lEndDim, pSubtaskInfo.splitInfo))
        return pmSuccess;

    bool lUpperTriangularComputation = (pSubtaskInfo.subtaskId < (pTaskInfo.subtaskCount/2));
    if(lUpperTriangularComputation)   // Upper Triangular Matrix
    {
        // Subscribe to block L00
        SUBSCRIBE_BLOCK(pTaskInfo.taskId, pTaskInfo.taskId, lTaskConf->matrixDim, pSubtaskInfo.subtaskId, pSubtaskInfo.splitInfo, READ_SUBSCRIPTION);

        // Subscribe to one horizontal block of matrix (with split)
        SUBSCRIBE_SPLIT_COL_BLOCK(pTaskInfo.taskId, pTaskInfo.taskId + 1 + pSubtaskInfo.subtaskId, lStartDim, lEndDim, lTaskConf->matrixDim, pSubtaskInfo.subtaskId, pSubtaskInfo.splitInfo, READ_WRITE_SUBSCRIPTION);
    }
    else    // Lower Triangular Matrix
    {
        size_t lStartingSubtask = (pTaskInfo.subtaskCount/2);

        // Subscribe to block U00
        SUBSCRIBE_BLOCK(pTaskInfo.taskId, pTaskInfo.taskId, lTaskConf->matrixDim, pSubtaskInfo.subtaskId, pSubtaskInfo.splitInfo, READ_SUBSCRIPTION);
        
        // Subscribe to one vertical block of matrix (with split)
        SUBSCRIBE_SPLIT_ROW_BLOCK(pTaskInfo.taskId + 1 + pSubtaskInfo.subtaskId - lStartingSubtask, pTaskInfo.taskId, lStartDim, lEndDim, lTaskConf->matrixDim, pSubtaskInfo.subtaskId, pSubtaskInfo.splitInfo, READ_WRITE_SUBSCRIPTION);
    }

	return pmSuccess;
}
    
pmStatus horizVertComp_cpu(pmTaskInfo pTaskInfo, pmDeviceInfo pDeviceInfo, pmSubtaskInfo pSubtaskInfo)
{
    if(pSubtaskInfo.splitInfo.splitCount && (pSubtaskInfo.memInfo[OUTPUT_MEM_INDEX].visibilityType == SUBSCRIPTION_COMPACT))
    {
        std::cout << "Compact subscription for splitted subtask not supported !!!";
        exit(1);
    }

	luTaskConf* lTaskConf = (luTaskConf*)(pTaskInfo.taskConf);
    
    size_t lStartDim, lEndDim;
    if(!GetSubtaskSplit(&lStartDim, &lEndDim, pSubtaskInfo.splitInfo))
        return pmSuccess;
    
    bool lUpperTriangularComputation = (pSubtaskInfo.subtaskId < (pTaskInfo.subtaskCount/2));
    
    size_t lOffsetElems = 0;
    int lSpanElems = 0;

    if(lUpperTriangularComputation)   // Upper Triangular Matrix (Solve A01 = L00 * U01) --- In col major orientation, upper triangular mtrix lies in lower half
    {
        if(pSubtaskInfo.memInfo[OUTPUT_MEM_INDEX].visibilityType == SUBSCRIPTION_NATURAL)
        {
            lOffsetElems = lStartDim * lTaskConf->matrixDim + BLOCK_OFFSET_IN_ELEMS(pTaskInfo.taskId, pTaskInfo.taskId + 1 + pSubtaskInfo.subtaskId, lTaskConf->matrixDim) - BLOCK_OFFSET_IN_ELEMS(pTaskInfo.taskId, pTaskInfo.taskId, lTaskConf->matrixDim);
            lSpanElems = (int)lTaskConf->matrixDim;
        }
        else
        {
            lOffsetElems = BLOCK_DIM * BLOCK_DIM;
            lSpanElems = (int)(BLOCK_DIM);
        }

        MATRIX_DATA_TYPE* lL00 = ((MATRIX_DATA_TYPE*)pSubtaskInfo.memInfo[OUTPUT_MEM_INDEX].ptr);
        MATRIX_DATA_TYPE* lU01 = lL00 + lOffsetElems;
        
        CBLAS_TRSM(CblasColMajor, CblasLeft, CblasLower, CblasNoTrans, CblasNonUnit, BLOCK_DIM, (int)(lEndDim - lStartDim), 1.0f, lL00, lSpanElems, lU01, lSpanElems);
    }
    else    // Lower Triangular Matrix (Solve A10 = L10 * U00) --- In col major orientation, lower triangular matrix lies in upper half
    {
        size_t lStartingSubtask = (pTaskInfo.subtaskCount/2);

        if(pSubtaskInfo.memInfo[OUTPUT_MEM_INDEX].visibilityType == SUBSCRIPTION_NATURAL)
        {
            lOffsetElems = lStartDim + BLOCK_OFFSET_IN_ELEMS(pTaskInfo.taskId + 1 + pSubtaskInfo.subtaskId - lStartingSubtask, pTaskInfo.taskId, lTaskConf->matrixDim) - BLOCK_OFFSET_IN_ELEMS(pTaskInfo.taskId, pTaskInfo.taskId, lTaskConf->matrixDim);
            lSpanElems = (int)lTaskConf->matrixDim;
        }
        else
        {
            lOffsetElems = BLOCK_DIM;
            lSpanElems = (int)(2 * BLOCK_DIM);
        }

        MATRIX_DATA_TYPE* lU00 = ((MATRIX_DATA_TYPE*)pSubtaskInfo.memInfo[OUTPUT_MEM_INDEX].ptr);
        MATRIX_DATA_TYPE* lL10 = lU00 + lOffsetElems;

        CBLAS_TRSM(CblasColMajor, CblasRight, CblasUpper, CblasNoTrans, CblasUnit, (int)(lEndDim - lStartDim), BLOCK_DIM, 1.0f, lU00, lSpanElems, lL10, lSpanElems);
    }

	return pmSuccess;
}
    
pmStatus diagCompDataDistribution(pmTaskInfo pTaskInfo, pmDeviceInfo pDeviceInfo, pmSubtaskInfo pSubtaskInfo)
{
	luTaskConf* lTaskConf = (luTaskConf*)(pTaskInfo.taskConf);
    
    size_t lDim = sqrtl(pTaskInfo.subtaskCount);
    size_t lRow = (pSubtaskInfo.subtaskId / lDim);
    size_t lCol = (pSubtaskInfo.subtaskId % lDim);

    size_t lStartDim, lEndDim;
    if(!GetSubtaskSplit(&lStartDim, &lEndDim, pSubtaskInfo.splitInfo))
        return pmSuccess;

    // Subscribe to one block from L10
    SUBSCRIBE_BLOCK(pTaskInfo.taskId + 1 + lRow, pTaskInfo.taskId, lTaskConf->matrixDim, pSubtaskInfo.subtaskId, pSubtaskInfo.splitInfo, READ_SUBSCRIPTION);
    
    // Subscribe to one block from U01 (with split)
    SUBSCRIBE_SPLIT_COL_BLOCK(pTaskInfo.taskId, pTaskInfo.taskId + 1 + lCol, lStartDim, lEndDim, lTaskConf->matrixDim, pSubtaskInfo.subtaskId, pSubtaskInfo.splitInfo, READ_SUBSCRIPTION);
    
    // Subscribe to one block of the matrix (with split)
    SUBSCRIBE_SPLIT_COL_BLOCK(pTaskInfo.taskId + 1 + lRow, pTaskInfo.taskId + 1 + lCol, lStartDim, lEndDim, lTaskConf->matrixDim, pSubtaskInfo.subtaskId, pSubtaskInfo.splitInfo, READ_WRITE_SUBSCRIPTION);
    
	return pmSuccess;
}
    
pmStatus diagComp_cpu(pmTaskInfo pTaskInfo, pmDeviceInfo pDeviceInfo, pmSubtaskInfo pSubtaskInfo)
{
    if(pSubtaskInfo.splitInfo.splitCount && (pSubtaskInfo.memInfo[OUTPUT_MEM_INDEX].visibilityType == SUBSCRIPTION_COMPACT))
    {
        std::cout << "Compact subscription for splitted subtask not supported !!!";
        exit(1);
    }

	luTaskConf* lTaskConf = (luTaskConf*)(pTaskInfo.taskConf);

    size_t lDim = sqrtl(pTaskInfo.subtaskCount);
    size_t lRow = (pSubtaskInfo.subtaskId / lDim);
    size_t lCol = (pSubtaskInfo.subtaskId % lDim);
    
    size_t lStartDim, lEndDim;
    if(!GetSubtaskSplit(&lStartDim, &lEndDim, pSubtaskInfo.splitInfo))
        return pmSuccess;

    size_t lOffsetElems1 = 0, lOffsetElems2 = 0;
    int lSpanElems1 = 0, lSpanElems2 = 0, lSpanElems3 = 0;

    if(pSubtaskInfo.memInfo[OUTPUT_MEM_INDEX].visibilityType == SUBSCRIPTION_NATURAL)
    {
        lOffsetElems1 = lStartDim * lTaskConf->matrixDim + BLOCK_OFFSET_IN_ELEMS(pTaskInfo.taskId, pTaskInfo.taskId + 1 + lCol, lTaskConf->matrixDim) - BLOCK_OFFSET_IN_ELEMS(pTaskInfo.taskId + 1 + lRow, pTaskInfo.taskId, lTaskConf->matrixDim);
    
        lOffsetElems2 = lStartDim * lTaskConf->matrixDim + BLOCK_OFFSET_IN_ELEMS(pTaskInfo.taskId + 1 + lRow, pTaskInfo.taskId + 1 + lCol, lTaskConf->matrixDim) - BLOCK_OFFSET_IN_ELEMS(pTaskInfo.taskId + 1 + lRow, pTaskInfo.taskId, lTaskConf->matrixDim);
        
        lSpanElems1 = lSpanElems2 = lSpanElems3 = (int)lTaskConf->matrixDim;
    }
    else
    {
        lOffsetElems1 = BLOCK_DIM * BLOCK_DIM;
        lOffsetElems2 = BLOCK_DIM * BLOCK_DIM + BLOCK_DIM;
        
        lSpanElems1 = (int)(BLOCK_DIM);
        lSpanElems2 = lSpanElems3 = (int)(2 * BLOCK_DIM);
    }

    MATRIX_DATA_TYPE* lL10 = ((MATRIX_DATA_TYPE*)pSubtaskInfo.memInfo[OUTPUT_MEM_INDEX].ptr);
    MATRIX_DATA_TYPE* lU01 = lL10 + lOffsetElems1;
    MATRIX_DATA_TYPE* lA11 = lL10 + lOffsetElems2;

    // Solve A11 = A11 - L10 * U01
    CBLAS_GEMM(CblasColMajor, CblasNoTrans, CblasNoTrans, BLOCK_DIM, (int)(lEndDim - lStartDim), BLOCK_DIM, -1.0f, lL10, lSpanElems1, lU01, lSpanElems2, 1.0f, lA11, lSpanElems3);
    
	return pmSuccess;
}

#define READ_NON_COMMON_ARGS \
    size_t lMatrixDim = DEFAULT_MATRIX_DIM; \
    FETCH_INT_ARG(lMatrixDim, pCommonArgs, argc, argv);

// Returns execution time on success; 0 on error
double DoSerialProcess(int argc, char** argv, int pCommonArgs)
{
	READ_NON_COMMON_ARGS

	double lStartTime = getCurrentTimeInSecs();
    
	serialLUDecomposition(gSerialOutput, lMatrixDim, lMatrixDim);
    
	double lEndTime = getCurrentTimeInSecs();
    
	return (lEndTime - lStartTime);
}

// Returns execution time on success; 0 on error
double DoSingleGpuProcess(int argc, char** argv, int pCommonArgs)
{
#ifdef BUILD_CUDA
	READ_NON_COMMON_ARGS

	double lStartTime = getCurrentTimeInSecs();
    
	singleGpuLUDecomposition(gParallelOutput, lMatrixDim);
    
	double lEndTime = getCurrentTimeInSecs();
    
	return (lEndTime - lStartTime);
#else
    return 0;
#endif
}
    
bool StageParallelTask(enum taskStage pStage, pmMemHandle pMemHandle, size_t pTaskId, size_t pMatrixDim, pmCallbackHandle pCallbackHandle, pmSchedulingPolicy pSchedulingPolicy)
{
    size_t lSubtasks = 0;
    
    switch(pStage)
    {
        case LU_DECOMPOSE:
            lSubtasks = 1;
            break;
        
        case HORZ_VERT_COMP:
        {
            size_t lBlocksPerDim = (pMatrixDim / BLOCK_DIM);
            lSubtasks = 2 * (lBlocksPerDim - pTaskId - 1);
            
            break;
        }

        case DIAGONAL_COMP:
        {
            size_t lBlocksPerDim = (pMatrixDim / BLOCK_DIM);
            lSubtasks = (lBlocksPerDim - pTaskId - 1) * (lBlocksPerDim - pTaskId - 1);
            
            break;
        }
    }

    CREATE_TASK(lSubtasks, pCallbackHandle, pSchedulingPolicy)

    luTaskConf lTaskConf;
    lTaskConf.matrixDim = pMatrixDim;
    
    pmTaskMem lTaskMem[MAX_MEM_INDICES];
    lTaskMem[OUTPUT_MEM_INDEX] = {pMemHandle, READ_WRITE, SUBSCRIPTION_OPTIMAL, true};

    lTaskDetails.taskConf = &lTaskConf;
    lTaskDetails.taskConfLength = sizeof(luTaskConf);
    lTaskDetails.taskMem = lTaskMem;
    lTaskDetails.taskMemCount = 1;
    lTaskDetails.taskId = pTaskId;

    if(pStage == HORZ_VERT_COMP || pStage == DIAGONAL_COMP)
        lTaskDetails.canSplitCpuSubtasks = true;

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

	// Input Mem contains input matrix
	// Output Mem contains lower triangular followed by upper triangular output matrix
	// Number of subtasks is equal to the matrix dim divided by block dim

    size_t lMatrixElems = lMatrixDim * lMatrixDim;
	size_t lMemSize = lMatrixElems * sizeof(MATRIX_DATA_TYPE);

    pmMemHandle lMemHandle;
	CREATE_MEM_2D(lMatrixDim, lMatrixDim * sizeof(MATRIX_DATA_TYPE), lMemHandle);
    
    pmRawMemPtr lRawMemPtr;
    pmGetRawMemPtr(lMemHandle, &lRawMemPtr);

	memcpy(lRawMemPtr, gParallelOutput, lMemSize);
    
    DistributeMemory(lMemHandle, BLOCK_DIST_2D_RANDOM, BLOCK_DIM, (unsigned int)lMatrixDim, (unsigned int)lMatrixDim, sizeof(MATRIX_DATA_TYPE), false);

	double lStartTime = getCurrentTimeInSecs();

    size_t lBlocksPerDim = (lMatrixDim / BLOCK_DIM);
    for(size_t task = 0; task < (lBlocksPerDim - 1); ++task)
    {
        // Decompose block (task, task) into lower and upper triangles
        StageParallelTask(LU_DECOMPOSE, lMemHandle, task, lMatrixDim, pCallbackHandle[0], pSchedulingPolicy);

        // Submit pmTask for row task, cols (task + 1, lBlocksPerDim) and for col task, rows (task + 1, lBlocksPerDim)
        StageParallelTask(HORZ_VERT_COMP, lMemHandle, task, lMatrixDim, pCallbackHandle[1], pSchedulingPolicy);
        
        // Submit pmTask for left over matrix rows (task + 1, lBlocksPerDim) cols (task + 1, lBlocksPerDim)
        StageParallelTask(DIAGONAL_COMP, lMemHandle, task, lMatrixDim, pCallbackHandle[2], pSchedulingPolicy);
    }
    
    // Decompose block (lBlocksPerDim - 1, lBlocksPerDim - 1) into lower and upper triangles
    StageParallelTask(LU_DECOMPOSE, lMemHandle, lBlocksPerDim - 1, lMatrixDim, pCallbackHandle[0], pSchedulingPolicy);

	double lEndTime = getCurrentTimeInSecs();
    
    if(pFetchBack)
    {
        SAFE_PM_EXEC( pmFetchMemory(lMemHandle) );
        memcpy(gParallelOutput, lRawMemPtr, lMemSize);
    }
    
    pmReleaseMemory(lMemHandle);
    
	return (lEndTime - lStartTime);
}

pmCallbacks DoSetDefaultCallbacks1()
{
	pmCallbacks lCallbacks;

	lCallbacks.dataDistribution = luDecompositionDataDistribution;
	lCallbacks.deviceSelection = NULL;
	lCallbacks.subtask_cpu = luDecomposition_cpu;
    
#ifdef BUILD_CUDA
	lCallbacks.subtask_gpu_custom = luDecomposition_cudaLaunchFunc;
#endif

	return lCallbacks;
}

pmCallbacks DoSetDefaultCallbacks2()
{
	pmCallbacks lCallbacks;

	lCallbacks.dataDistribution = horizVertCompDataDistribution;
	lCallbacks.deviceSelection = NULL;
	lCallbacks.subtask_cpu = horizVertComp_cpu;
    
#ifdef BUILD_CUDA
	lCallbacks.subtask_gpu_custom = horizVertComp_cudaLaunchFunc;
#endif
    
	return lCallbacks;
}

pmCallbacks DoSetDefaultCallbacks3()
{
	pmCallbacks lCallbacks;

	lCallbacks.dataDistribution = diagCompDataDistribution;
	lCallbacks.deviceSelection = NULL;
	lCallbacks.subtask_cpu = diagComp_cpu;
    
#ifdef BUILD_CUDA
	lCallbacks.subtask_gpu_custom = diagComp_cudaLaunchFunc;
#endif
    
	return lCallbacks;
}

// Returns 0 on success; non-zero on failure
int DoInit(int argc, char** argv, int pCommonArgs)
{
	READ_NON_COMMON_ARGS
    
    if(lMatrixDim < BLOCK_DIM)
    {
        std::cout << "Error: Matrix dimension must be atleast " << BLOCK_DIM << std::endl;
        exit(1);
    }
    
	srand((unsigned int)time(NULL));
    
    size_t lMatrixElems = lMatrixDim * lMatrixDim;
	gSerialOutput = new MATRIX_DATA_TYPE[lMatrixElems];
	gParallelOutput = new MATRIX_DATA_TYPE[lMatrixElems];

    MATRIX_DATA_TYPE lFactor = (1.0 + (MATRIX_DATA_TYPE)rand());
    
    do
    {
        for(size_t i = 0; i < lMatrixElems; ++i)
            gSerialOutput[i] = gParallelOutput[i] = (1.0 + (MATRIX_DATA_TYPE)rand()) / lFactor; // i + 1;
    } while(isSingularMatrix(gSerialOutput, lMatrixDim));
    
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
#ifdef MATRIX_DATA_TYPE_DOUBLE
	READ_NON_COMMON_ARGS
    
//    for(size_t i = 0; i < (lMatrixDim * lMatrixDim); ++i)
//        std::cout << gSerialOutput[i] << " " << gParallelOutput[i] << std::endl;

    std::unique_ptr<MATRIX_DATA_TYPE> lSerial = reproduceNonDecomposedMatrix(gSerialOutput, lMatrixDim);
    std::unique_ptr<MATRIX_DATA_TYPE> lParallel = reproduceNonDecomposedMatrix(gParallelOutput, lMatrixDim);
    
    MATRIX_DATA_TYPE lSerialNormal = findFrobeniusNormal(lSerial.get(), lMatrixDim);
    MATRIX_DATA_TYPE lParallelNormal = findFrobeniusNormal(lParallel.get(), lMatrixDim);
    
    return (fabsf(lSerialNormal - lParallelNormal)/lSerialNormal > 1e-5);
#else
    std::cout << "Results not compared in single precision" << std::endl;
    return 0;
#endif
}

/**	Non-common args
 *	1. log 2 (matrix dim)
 */
int main(int argc, char** argv)
{
    callbackStruct lStruct[3] = { {DoSetDefaultCallbacks1, "LU_S1"}, {DoSetDefaultCallbacks2, "LU_S2"}, {DoSetDefaultCallbacks3, "LU_S3"} };

	commonStart(argc, argv, DoInit, DoSerialProcess, DoSingleGpuProcess, DoParallelProcess, DoCompare, DoDestroy, lStruct, 3);
    
	commonFinish();

	return 0;
}

}
