// --------------------------------------------------------------------------
//
// File
//		Name:    Timer.cpp
//		Purpose: Generic timers which execute arbitrary code when
//			 they expire.
//		Created: 5/11/2006
//
// --------------------------------------------------------------------------

#include "Box.h"

#include <signal.h>

#include "Timer.h"

#include "MemLeakFindOn.h"

std::vector<Timer*>* Timers::spTimers = NULL;
bool Timers::sRescheduleNeeded = false;

// --------------------------------------------------------------------------
//
// Function
//		Name:    static void Timers::Init()
//		Purpose: Initialise timers, prepare signal handler
//		Created: 5/11/2006
//
// --------------------------------------------------------------------------
void Timers::Init()
{
	ASSERT(!spTimers);
	
	#if defined WIN32 && ! defined PLATFORM_CYGWIN
		// no support for signals at all
		InitTimer();
		SetTimerHandler(Timers::SignalHandler);
	#else
		ASSERT(::signal(SIGALRM, Timers::SignalHandler) == 0);
	#endif // WIN32 && !PLATFORM_CYGWIN
	
	spTimers = new std::vector<Timer*>;
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    static void Timers::Cleanup()
//		Purpose: Clean up timers, stop signal handler
//		Created: 6/11/2006
//
// --------------------------------------------------------------------------
void Timers::Cleanup()
{
	ASSERT(spTimers);
	
	#if defined WIN32 && ! defined PLATFORM_CYGWIN
		// no support for signals at all
		FiniTimer();
		SetTimerHandler(NULL);
	#else
		struct itimerval timeout;
		memset(&timeout, 0, sizeof(timeout));
		ASSERT(::setitimer(ITIMER_REAL, &timeout, NULL) == 0);
		ASSERT(::signal(SIGALRM, NULL) == Timers::SignalHandler);
	#endif // WIN32 && !PLATFORM_CYGWIN

	spTimers->clear();
	delete spTimers;
	spTimers = NULL;
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    static void Timers::Add(Timer&)
//		Purpose: Add a new timer to the set, and reschedule next wakeup
//		Created: 5/11/2006
//
// --------------------------------------------------------------------------
void Timers::Add(Timer& rTimer)
{
	ASSERT(spTimers);
	spTimers->push_back(&rTimer);
	Reschedule();
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    static void Timers::Remove(Timer&)
//		Purpose: Removes the timer from the set (preventing it from
//			 being called) and reschedule next wakeup
//		Created: 5/11/2006
//
// --------------------------------------------------------------------------
void Timers::Remove(Timer& rTimer)
{
	ASSERT(spTimers);

	bool restart = true;
	while (restart)
	{
		restart = false;
		for (std::vector<Timer*>::iterator i = spTimers->begin();
			i != spTimers->end(); i++)
		{
			if (&rTimer == *i)
			{
				spTimers->erase(i);
				restart = true;
				break;
			}
		}
	}
		
	Reschedule();
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    static void Timers::Reschedule()
//		Purpose: Recalculate when the next wakeup is due
//		Created: 5/11/2006
//
// --------------------------------------------------------------------------
void Timers::Reschedule()
{
	ASSERT(spTimers);

	// Clear the reschedule-needed flag to false before we start.
	// If a timer event occurs while we are scheduling, then we
	// may or may not need to reschedule again, but this way
	// we will do it anyway.
	sRescheduleNeeded = false;

	box_time_t timeNow = GetCurrentBoxTime();

	// scan for, trigger and remove expired timers. Removal requires
	// us to restart the scan each time, due to std::vector semantics.
	bool restart = true;
	while (restart)
	{
		restart = false;

		for (std::vector<Timer*>::iterator i = spTimers->begin();
			i != spTimers->end(); i++)
		{
			Timer& rTimer = **i;
			int64_t timeToExpiry = rTimer.GetExpiryTime() - timeNow;
		
			if (timeToExpiry <= 0)
			{
				TRACE3("%d.%d: timer %p has expired, "
					"triggering it\n",
					(int)(timeNow / 1000000), 
					(int)(timeNow % 1000000),
					*i);
				rTimer.OnExpire();
				spTimers->erase(i);
				restart = true;
				break;
			}
			else
			{
				TRACE5("%d.%d: timer %p has not expired, "
					"triggering in %d.%d seconds\n",
					(int)(timeNow / 1000000), 
					(int)(timeNow % 1000000),
					*i,
					(int)(timeToExpiry / 1000000),
					(int)(timeToExpiry % 1000000));
			}
		}
	}

	// Now the only remaining timers should all be in the future.
	// Scan to find the next one to fire (earliest deadline).
			
	int64_t timeToNextEvent = 0;

	for (std::vector<Timer*>::iterator i = spTimers->begin();
		i != spTimers->end(); i++)
	{
		Timer& rTimer = **i;
		int64_t timeToExpiry = rTimer.GetExpiryTime() - timeNow;

		if (timeToExpiry <= 0)
		{
			timeToExpiry = 1;
		}
		
		if (timeToNextEvent == 0 || timeToNextEvent > timeToExpiry)
		{
			timeToNextEvent = timeToExpiry;
		}
	}
	
	ASSERT(timeToNextEvent >= 0);
	
	struct itimerval timeout;
	memset(&timeout, 0, sizeof(timeout));
	
	timeout.it_value.tv_sec  = BoxTimeToSeconds(timeToNextEvent);
	timeout.it_value.tv_usec = (int)
		(BoxTimeToMicroSeconds(timeToNextEvent) % MICRO_SEC_IN_SEC);

	if(::setitimer(ITIMER_REAL, &timeout, NULL) != 0)
	{
		TRACE0("WARNING: couldn't initialise timer\n");
		THROW_EXCEPTION(CommonException, Internal)
	}
}

// --------------------------------------------------------------------------
//
// Function
//		Name:    static void Timers::SignalHandler(unused)
//		Purpose: Called as signal handler. Nothing is safe in a signal
//			 handler, not even traversing the list of timers, so
//			 just request a reschedule in future, which will do
//			 that for us, and trigger any expired timers at that
//			 time.
//		Created: 5/11/2006
//
// --------------------------------------------------------------------------
void Timers::SignalHandler(int iUnused)
{
	// ASSERT(spTimers);
	Timers::RequestReschedule();
}

Timer::Timer(size_t timeoutSecs)
: mExpires(GetCurrentBoxTime() + SecondsToBoxTime(timeoutSecs)),
  mExpired(false)
{
	#ifndef NDEBUG
	struct timeval tv;
	gettimeofday(&tv, NULL);
	if (timeoutSecs == 0)
	{
		TRACE4("%d.%d: timer %p initialised for %d secs, "
			"will not fire\n", tv.tv_sec, tv.tv_usec, this, 
			timeoutSecs);
	}
	else
	{
		TRACE6("%d.%d: timer %p initialised for %d secs, "
			"to fire at %d.%d\n", tv.tv_sec, tv.tv_usec, this, 
			timeoutSecs, (int)(mExpires / 1000000), 
			(int)(mExpires % 1000000));
	}
	#endif

	if (timeoutSecs == 0)
	{
		mExpires = 0;
	}
	else
	{
		Timers::Add(*this);
	}
}

Timer::~Timer()
{
	#ifndef NDEBUG
	struct timeval tv;
	gettimeofday(&tv, NULL);
	TRACE3("%d.%d: timer %p destroyed, will not fire\n",
		tv.tv_sec, tv.tv_usec, this);
	#endif

	Timers::Remove(*this);
}

Timer::Timer(const Timer& rToCopy)
: mExpires(rToCopy.mExpires),
  mExpired(rToCopy.mExpired)
{
	#ifndef NDEBUG
	struct timeval tv;
	gettimeofday(&tv, NULL);
	if (mExpired)
	{
		TRACE4("%d.%d: timer %p initialised from timer %p, "
			"already expired, will not fire\n", tv.tv_sec, 
			tv.tv_usec, this, &rToCopy);
	}
	else if (mExpires == 0)
	{
		TRACE4("%d.%d: timer %p initialised from timer %p, "
			"will not fire\n", tv.tv_sec, tv.tv_usec, this, 
			&rToCopy);
	}
	else
	{
		TRACE6("%d.%d: timer %p initialised from timer %p, "
			"to fire at %d.%d\n", tv.tv_sec, tv.tv_usec, this, 
			&rToCopy, (int)(mExpires / 1000000), 
			(int)(mExpires % 1000000));
	}
	#endif

	if (!mExpired && mExpires != 0)
	{
		Timers::Add(*this);
	}
}

Timer& Timer::operator=(const Timer& rToCopy)
{
	#ifndef NDEBUG
	struct timeval tv;
	gettimeofday(&tv, NULL);
	if (rToCopy.mExpired)
	{
		TRACE4("%d.%d: timer %p initialised from timer %p, "
			"already expired, will not fire\n", tv.tv_sec, 
			tv.tv_usec, this, &rToCopy);
	}
	else if (rToCopy.mExpires == 0)
	{
		TRACE4("%d.%d: timer %p initialised from timer %p, "
			"will not fire\n", tv.tv_sec, tv.tv_usec, this, 
			&rToCopy);
	}
	else
	{
		TRACE6("%d.%d: timer %p initialised from timer %p, "
			"to fire at %d.%d\n", tv.tv_sec, tv.tv_usec, this, 
			&rToCopy, (int)(rToCopy.mExpires / 1000000), 
			(int)(rToCopy.mExpires % 1000000));
	}
	#endif

	Timers::Remove(*this);
	mExpires = rToCopy.mExpires;
	mExpired = rToCopy.mExpired;
	if (!mExpired && mExpires != 0)
	{
		Timers::Add(*this);
	}
	return *this;
}

void Timer::OnExpire()
{
	#ifndef NDEBUG
	struct timeval tv;
	gettimeofday(&tv, NULL);
	TRACE3("%d.%d: timer %p fired\n", tv.tv_sec, tv.tv_usec, this);
	#endif

	mExpired = true;
}
