
#include "pmSignalWait.h"

namespace pm
{

/* class pmPThreadSignalWait */
pmPThreadSignalWait::pmPThreadSignalWait()
{
	THROW_ON_NON_ZERO_RET_VAL( pthread_mutex_init(&mMutex, NULL), pmThreadFailure, pmThreadFailure::MUTEX_INIT_FAILURE );
	THROW_ON_NON_ZERO_RET_VAL( pthread_cond_init(&mCondVariable, NULL), pmThreadFailure, pmThreadFailure::COND_VAR_INIT_FAILURE );
	mCondEnforcer = false;
}

pmPThreadSignalWait::~pmPThreadSignalWait()
{
	THROW_ON_NON_ZERO_RET_VAL( pthread_mutex_destroy(&mMutex), pmThreadFailure, pmThreadFailure::MUTEX_DESTROY_FAILURE );
	THROW_ON_NON_ZERO_RET_VAL( pthread_cond_destroy(&mCondVariable), pmThreadFailure, pmThreadFailure::COND_VAR_DESTROY_FAILURE );
}

pmStatus pmPThreadSignalWait::Wait()
{
	while(!mCondEnforcer)
		THROW_ON_NON_ZERO_RET_VAL( pthread_cond_wait(&mCondVariable, &mMutex), pmThreadFailure, pmThreadFailure::COND_VAR_WAIT_FAILURE );

	mCondEnforcer = false;

	return pmSuccess;
}

pmStatus pmPThreadSignalWait::Signal()
{
	THROW_ON_NON_ZERO_RET_VAL( pthread_mutex_lock(&mMutex), pmThreadFailure, pmThreadFailure::MUTEX_LOCK_FAILURE );
	mCondEnforcer = true;

	THROW_ON_NON_ZERO_RET_VAL( pthread_cond_signal(&mCondVariable), pmThreadFailure, pmThreadFailure::COND_VAR_SIGNAL_FAILURE );
	THROW_ON_NON_ZERO_RET_VAL( pthread_mutex_unlock(&mMutex), pmThreadFailure, pmThreadFailure::MUTEX_UNLOCK_FAILURE );

	return pmSuccess;
}

}