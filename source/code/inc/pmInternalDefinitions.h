
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

#ifndef __PM_INTERNAL_DEFINITIONS__
#define __PM_INTERNAL_DEFINITIONS__

#define PMLIB_VERSION "1.0.0" /* Format MajorVersion.MinorVersion.Update */

#define SYSTEM_CONFIGURATION_HEADER <unistd.h>

/** 
 * The actual implementations to be used in the build for abstract factory based classes
*/
#define NETWORK_IMPLEMENTATION_CLASS pmMPI
#define CLUSTER_IMPLEMENTATION_CLASS pmClusterMPI

#ifdef UNIX

#define TIMER_IMPLEMENTATION_CLASS pmLinuxTimer
#define TIMER_IMPLEMENTATION_HEADER <sys/time.h>

#define VM_IMPLEMENTATION_HEADER1 <sys/mman.h>
#define VM_IMPLEMENTATION_HEADER2 <signal.h>
#define VM_IMPLEMENTATION_HEADER3 <unistd.h>

#define MEMORY_MANAGER_IMPLEMENTATION_CLASS pmLinuxMemoryManager

#define THREADING_IMPLEMENTATION_CLASS pmPThread	// Define the implementation header in THREADING_IMPLEMENTATION_HEADER
#define TLS_IMPLEMENTATION_CLASS pmPThreadTls	// Define the implementation header in THREADING_IMPLEMENTATION_HEADER
#define SIGNAL_WAIT_IMPLEMENTATION_CLASS pmPThreadSignalWait	// Define the implementation header in THREADING_IMPLEMENTATION_HEADER
#define RESOURCE_LOCK_IMPLEMENTATION_CLASS pmPThreadResourceLock	// Define the implementation header in THREADING_IMPLEMENTATION_HEADER
#define RW_RESOURCE_LOCK_IMPLEMENTATION_CLASS pmPThreadRWResourceLock	// Define the implementation header in THREADING_IMPLEMENTATION_HEADER
#define THREADING_IMPLEMENTATION_HEADER <pthread.h>	// Used alongwith THREADING_IMPLEMENTATION_CLASS, SIGNAL_WAIT_IMPLEMENTATION_CLASS & RESOURCE_LOCK_IMPLEMENTATION_CLASS

#else

#define TIMER_IMPLEMENTATION_CLASS pmWinTimer
#define TIMER_IMPLEMENTATION_HEADER 
#define STANDARD_ERROR_HEADER

#define VM_IMPLEMENTATION_HEADER1 
#define VM_IMPLEMENTATION_HEADER2 
#define VM_IMPLEMENTATION_HEADER3 

#define MEMORY_MANAGER_IMPLEMENTATION_CLASS pmWinMemoryManager

#define THREADING_IMPLEMENTATION_CLASS pmWinThread	// Define the implementation header in THREADING_IMPLEMENTATION_HEADER
#define SIGNAL_WAIT_IMPLEMENTATION_CLASS pmWinThreadSignalWait	// Define the implementation header in THREADING_IMPLEMENTATION_HEADER
#define RESOURCE_LOCK_IMPLEMENTATION_CLASS pmWinThreadResourceLock	// Define the implementation header in THREADING_IMPLEMENTATION_HEADER
#define THREADING_IMPLEMENTATION_HEADER 	// Used alongwith THREADING_IMPLEMENTATION_CLASS, SIGNAL_WAIT_IMPLEMENTATION_CLASS & RESOURCE_LOCK_IMPLEMENTATION_CLASS

#endif

#define THROW_ON_NON_ZERO_RET_VAL(x, y, z) { int __ret_val__ = x; if(__ret_val__) PMTHROW(y(z, __ret_val__)); }

/* This code is taken from http://www.fefe.de/intof.html */
#define __HALF_MAX_SIGNED(type) ((type)1 << (sizeof(type)*8-2))
#define __MAX_SIGNED(type) (__HALF_MAX_SIGNED(type) - 1 + __HALF_MAX_SIGNED(type))
#define __MIN_SIGNED(type) (-1 - __MAX_SIGNED(type))

#define __MIN(type) ((type)-1 < 1?__MIN_SIGNED(type):(type)0)
#define __MAX(type) ((type)~__MIN(type))
///////////////////////////////////////////////////////////

#define MPI_TRANSFER_MAX_LIMIT __MAX(int)

const unsigned short RESERVED_PRIORITY = 0;
const unsigned short MAX_CONTROL_PRIORITY = RESERVED_PRIORITY+1;
const unsigned short MAX_PRIORITY_LEVEL = MAX_CONTROL_PRIORITY+1;	// 0 is used for control messages
const unsigned short MIN_PRIORITY_LEVEL = __MAX(unsigned short);
const unsigned short DEFAULT_PRIORITY_LEVEL = MAX_PRIORITY_LEVEL;

const unsigned short TASK_MULTI_ASSIGN_FLAG_VAL = 0x0001;   // LSB
const unsigned short TASK_SHOULD_OVERLAP_COMPUTE_COMMUNICATION_FLAG_VAL = 0x0002;
const unsigned short TASK_CAN_FORCIBLY_CANCEL_SUBTASKS_FLAG_VAL = 0x0004;
const unsigned short TASK_CAN_SPLIT_CPU_SUBTASKS_FLAG_VAL = 0x008;
const unsigned short TASK_CAN_SPLIT_GPU_SUBTASKS_FLAG_VAL = 0x0010;
const unsigned short TASK_HAS_CUDA_CACHE_ENABLED_FLAG_VAL = 0x0020;  // Even if SUPPORT_CUDA is not enabled, define this
const unsigned short TASK_SUPPRESS_LOGS_FLAG_VAL = 0x0040;  // This is disabled by default

#ifdef SUPPORT_CUDA
const unsigned short DEFAULT_TASK_FLAGS_VAL = (TASK_MULTI_ASSIGN_FLAG_VAL | TASK_SHOULD_OVERLAP_COMPUTE_COMMUNICATION_FLAG_VAL | TASK_CAN_FORCIBLY_CANCEL_SUBTASKS_FLAG_VAL | TASK_HAS_CUDA_CACHE_ENABLED_FLAG_VAL);
#else
const unsigned short DEFAULT_TASK_FLAGS_VAL = (TASK_MULTI_ASSIGN_FLAG_VAL | TASK_SHOULD_OVERLAP_COMPUTE_COMMUNICATION_FLAG_VAL | TASK_CAN_FORCIBLY_CANCEL_SUBTASKS_FLAG_VAL);
#endif

#ifdef SUPPORT_CUDA
const unsigned int CUDA_CHUNK_SIZE_MULTIPLIER_PER_GB = (64 * 1024 * 1024); // minimum 64 MB chunk per GB
const unsigned int PINNED_CHUNK_SIZE_MULTIPLIER_PER_GB = CUDA_CHUNK_SIZE_MULTIPLIER_PER_GB; // minimum 64 MB chunk per GB
const unsigned int SCRATCH_CHUNK_SIZE_MULTIPLIER_PER_GB = (32 * 1024 * 1024); // minimum 32 MB chunk per GB
const unsigned int MIN_UNALLOCATED_CUDA_MEM_SIZE = (4 * 1024 * 1024);  // in bytes
#endif

#define DEFAULT_SCHEDULING_MODEL scheduler::PULL

#define SLOW_START_SCHEDULING_INITIAL_SUBTASK_COUNT 1	 // must be a power of 2
#define SLOW_START_SCHEDULING_UPPER_LIMIT_EXEC_TIME_PER_ALLOCATION 15	// in seconds
#define SLOW_START_SCHEDULING_LOWER_LIMIT_EXEC_TIME_PER_ALLOCATION 8	// in seconds

const double MA_WAIT_FACTOR = 1.05;  // If local stub's exec rate is zero, do not allow multi-assign till it has executed the subtask for 5% more time than requestor
const double MA_WAIT_FACTOR_LENIENT = 0.95;  // If local stub's exec rate is zero, do not allow multi-assign till it has executed the subtask for 95% time as requestor (less than 100% is reqd. for pipeline continuation of GPUs)
const double SUBTASK_TRANSFER_OVERHEAD = 1.05;  // Assuming 5% overhead for steal/multi-assign

#define MAX_SUBTASK_MULTI_ASSIGN_COUNT 2    // Max no. of devices to which a subtask may be assigned at any given time
#define MAX_STEAL_CYCLES_PER_DEVICE 5   // Max no. of steal attempts from a device to any other device

#define GET_VM_PAGE_START_ADDRESS(memAddr, pageSize) (memAddr - (memAddr % pageSize))


/* Pull scheduling controls */
//#define RANDOMIZE_PULL_ASSIGNMENTS


/* Lazy address space controls */
//#define SUPPORT_LAZY_MEMORY
#ifdef SUPPORT_LAZY_MEMORY
    #define LAZY_FORWARD_PREFETCH_PAGE_COUNT 5
#endif


/* Memory transfer controls */
//#define GROUP_SCATTERED_REQUESTS
#define PROCESS_METADATA_RECEIVE_IN_NETWORK_THREAD
#define BLOCK_PREFETCH_OF_ANTICIPATED_SUBTASKS


/* Subtask splitting controls */
#define SUPPORT_SPLIT_SUBTASKS
#define ENABLE_DYNAMIC_SPLITTING
//#define FORCE_START_WITH_ONE_SUBTASK_PER_SPLIT_GROUP


/* Pipelining controls */
#define SUPPORT_COMPUTE_COMMUNICATION_OVERLAP
#define SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
//#define PRE_DETERMINE_MAX_COLLECTIVELY_EXECUTABLE_CUDA_SUBTASKS
//#define BREAK_PIPELINE_ON_RESOURCE_EXHAUSTION


/* Stealing controls */
#ifdef SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP
    #define PROACTIVE_STEAL_REQUESTS    // This can be enabled even if the macro SUPPORT_CUDA_COMPUTE_MEM_TRANSFER_OVERLAP is undefined, it mostly benefits GPUs executing a pipeline of subtasks
#endif
#define ENABLE_TWO_LEVEL_STEALING
#ifdef ENABLE_TWO_LEVEL_STEALING
    #define USE_STEAL_AGENT_PER_NODE
#endif

// The following steal controls exist for experimental purposes only
//#define ENABLE_HALF_STEAL_CHUNK_SELECTION
#ifndef ENABLE_TWO_LEVEL_STEALING
//    #define ENABLE_ROUND_ROBIN_VICTIM_SELECTION
//    #define ENABLE_CPU_FIRST_VICTIM_SELECTION
    #ifdef SUPPORT_CUDA
//        #define ENABLE_GPU_FIRST_VICTIM_SELECTION
    #endif
#endif

#ifdef USE_STEAL_AGENT_PER_NODE
#ifndef ENABLE_HALF_STEAL_CHUNK_SELECTION
#ifdef PROACTIVE_STEAL_REQUESTS
    #define ENABLE_DYNAMIC_AGGRESSION
#endif
#endif
#endif

#define USE_AFFINITY_IN_STEAL


/* Affinity Controls */
//#define MACHINES_PICK_BEST_SUBTASKS // defines whether subtasks choose best machines or vice versa
#ifdef MACHINES_PICK_BEST_SUBTASKS
//    #define GENERALIZED_RESIDUAL_PROFIT_ASSIGNMENT
#endif
//#define CENTRALIZED_AFFINITY_COMPUTATION
#ifdef USE_STEAL_AGENT_PER_NODE
//    #define USE_DYNAMIC_AFFINITY
#endif


/* Reduction Controls */
//#define USE_MPI_REDUCE
#ifdef USE_MPI_REDUCE
#define PRE_CREATE_SUB_COMMUNICATORS    // MPI-2 requires all nodes to call MPI_Create_Comm call even if they are not part of the subcommunicator
#else
#ifndef __clang__
#define USE_OMP_FOR_REDUCTION
#endif
#endif

//#define TURN_OFF_GPU_SENTINEL_COMPRESSION
//#define TURN_OFF_NETWORK_SENTINEL_COMPRESSION
const unsigned int CUDA_SENTINEL_COMPRESSION_THRESHOLD = (1024 * 1024 * 1024);  // 1 GB
const float CUDA_SENTINEL_COMPRESSION_MAX_NON_SENTINELS = 0.4;


/* Utility controls */
#define BUILD_FOR_PMLIB_ANALYZER


#define PROPORTIONAL_SCHEDULING_CONF_FILE "propSchedConf.txt"


/* Diagnostics */
//#define RECORD_LOCK_ACQUISITIONS
//#define TRACK_MEMORY_ALLOCATIONS
//#define TRACK_MEMORY_REQUESTS
//#define TRACK_SUBTASK_EXECUTION
//#define TRACK_SUBTASK_EXECUTION_VERBOSE
//#define TRACK_SUBTASK_STEALS
//#define TRACK_MULTI_ASSIGN
//#define TRACK_MUTEX_TIMINGS
//#define TRACK_MEM_COPIES
//#define ENABLE_TASK_PROFILING
//#define ENABLE_MEM_PROFILING
//#define ENABLE_ACCUMULATED_TIMINGS
//#define DUMP_THREADS
//#define DUMP_SHADOW_MEM
//#define DUMP_NETWORK_STATS
//#define DUMP_TASK_EXEC_STATS
//#define DUMP_SCHEDULER_EVENT
//#define DUMP_EVENT_TIMELINE
//#define DUMP_MPI_CALLS
//#define DUMP_DATA_TRANSFER_FREQUENCY    // Only supported for 2D address spaces; this logging can be expensive, should be turned on only for diagnosis
#define DUMP_SUBTASK_EXECUTION_PROFILE
#define DUMP_EXCEPTION_BACKTRACE
//#define DUMP_CUDA_CACHE_STATISTICS
//#define DUMP_AFFINITY_DATA
//#define DUMP_DATA_COMPRESSION_STATISTICS

#define EXIT_ON_EXCEPTION

#ifdef TRACK_MUTEX_TIMINGS
    #ifndef ENABLE_ACCUMULATED_TIMINGS
        #define ENABLE_ACCUMULATED_TIMINGS
    #endif
#endif

#ifdef BUILD_FOR_PMLIB_ANALYZER
    #define ENABLE_TASK_PROFILING
    #define DUMP_TASK_EXEC_STATS
    #define DUMP_EVENT_TIMELINE
    #define DUMP_SUBTASK_EXECUTION_PROFILE
    #define DUMP_CUDA_CACHE_STATISTICS
    #define DUMP_DATA_COMPRESSION_STATISTICS
#endif

#if defined(DUMP_EVENT_TIMELINE) || defined(ENABLE_ACCUMULATED_TIMINGS)
    #define SERIALIZE_DEFERRED_LOGS
#endif

#ifdef _DEBUG
    #define DUMP_EXCEPTION_BACKTRACE
#endif

#ifdef SUPPORT_CUDA
    //#define CREATE_EXPLICIT_CUDA_CONTEXTS

    #define CUDA_CACHE_EVICTION_POLICY LEAST_RECENTLY_USED
    //#define CUDA_CACHE_EVICTION_POLICY MOST_RECENTLY_USED
    //#define CUDA_CACHE_EVICTION_POLICY LEAST_FREQUENTLY_USED
    //#define CUDA_CACHE_EVICTION_POLICY MOST_FREQUENTLY_USED
    //#define CUDA_CACHE_EVICTION_POLICY RANDOM_EVICTION
#else
    #undef SUPPORT_SPLIT_SUBTASKS
#endif

#endif



