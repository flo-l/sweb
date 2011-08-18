/**
 * @file Scheduler.cpp
 */

#include "Scheduler.h"
#include "Thread.h"
#include "arch_panic.h"
#include "ArchThreads.h"
#include "console/kprintf.h"
#include "ArchInterrupts.h"
#include "mm/KernelMemoryManager.h"
#include <ustl/ulist.h>

ArchThreadInfo *currentThreadInfo;
Thread *currentThread;

Scheduler *Scheduler::instance_=0;

Scheduler *Scheduler::instance()
{
  return instance_;
}

/**
 * @class IdleThread
 * periodically calls cleanUpDeadThreads
 */
class IdleThread : public Thread
{
  public:

    /**
     * Constructor
     * @return IdleThread instance
     */
    IdleThread() : Thread("IdleThread")
    {
    }

    /**
     * calls cleanUpDeadThreads
     */
    virtual void Run()
    {
      uint32 last_ticks = 0;
      uint32 new_ticks = 0;
      while ( 1 )
      {
        Scheduler::instance()->cleanupDeadThreads();
        new_ticks = Scheduler::instance()->getTicks();
        if (new_ticks == last_ticks)
        {
          last_ticks = new_ticks + 1;
          __asm__ __volatile__ ( "hlt" );
        }
        else
        {
          last_ticks = new_ticks;
          Scheduler::instance()->yield();
        }
      }
    }
};

void Scheduler::createScheduler()
{
  if (instance_)
    return;

  instance_ = new Scheduler();

  // create idle thread, this one really does not do too much

  Thread *idle = new IdleThread();
  instance_->addNewThread ( idle );
}

Scheduler::Scheduler()
{
  kill_old_=false;
  block_scheduling_=0;
  block_scheduling_extern_=0;
  ticks_=0;
}

void Scheduler::addNewThread ( Thread *thread )
{
  //new Thread gets scheduled next
  //also gets added to front as not to interfere with remove or xchange

  lockScheduling();
  debug ( SCHEDULER,"addNewThread: %x  %d:%s\n",thread,thread->getPID(), thread->getName() );
  waitForFreeKMMLock();
  threads_.push_front ( thread );
  unlockScheduling();
}

void Scheduler::removeCurrentThread()
{
  lockScheduling();
  debug ( SCHEDULER,"removeCurrentThread: %x %d:%s, threads_.size() %d\n",currentThread,currentThread->getPID(),currentThread->getName(),threads_.size() );
  waitForFreeKMMLock();
  if ( threads_.size() > 1 )
  {
    threads_.remove(currentThread);
    /*for ( uint32 c=0; c< threads_.size(); ++c )
    {
      tmp_thread = threads_.back();
      if ( tmp_thread == currentThread )
      {
        threads_.popBack();
        break;
      }
      threads_.rotateFront();
    }*/
  }
  unlockScheduling();
}

void Scheduler::sleep()
{
  currentThread->state_=Sleeping;
  //if we somehow stupidly go to sleep, block is automatically removed
  //we might break a lock in doing so, but that's still better than having no chance
  //of recovery whatsoever.
  unlockScheduling();
  yield();
}

void Scheduler::sleepAndRelease ( SpinLock &lock )
{
  lockScheduling();
  currentThread->state_=Sleeping;
  lock.release();
  unlockScheduling();
  yield();
}

void Scheduler::sleepAndRelease ( Mutex &lock )
{
  lockScheduling();
  waitForFreeKMMLockAndFreeSpinLock(lock.spinlock_);
  currentThread->state_=Sleeping;
  lock.release();
  unlockScheduling();
  yield();
}

// beware using this funktion!
void Scheduler::sleepAndRestoreInterrupts ( bool interrupts )
{
  // why do it get the feeling that misusing this function
  // can mean a serious shot in the foot ?
  currentThread->state_=Sleeping;
  if ( interrupts )
  {
    ArchInterrupts::enableInterrupts();
    assert ( block_scheduling_extern_==0 );
    yield();
  }
}

void Scheduler::wake ( Thread* thread_to_wake )
{
  thread_to_wake->state_=Running;
}

void Scheduler::startThreadHack()
{
  currentThread->Run();
}

uint32 Scheduler::schedule()
{
  if ( testLock() || block_scheduling_extern_>0 )
  {
    //no scheduling today...
    //keep currentThread as it was
    //and stay in Kernel Kontext
    debug ( SCHEDULER,"schedule: currently blocked\n" );
    return 0;
  }

  Thread* previousThread = currentThread;
  do
  {
    // WARNING: do not read currentThread before is has been set here
    //          the first time scheduler is called.
    //          before this, currentThread may be 0 !!
    currentThread = threads_.front();

    if ( kill_old_ == false && currentThread->state_ == ToBeDestroyed )
      kill_old_=true;

    //this operation doesn't allocate or delete any kernel memory
    //threads_.rotateBack();
    //ustl::rotate(threads_.begin(),threads_.end(), threads_.end());
    threads_.push_back(threads_.front());
    threads_.pop_front();
    //swap(*threads_.begin(), *threads_.end());
    if ((currentThread == previousThread) && (currentThread->state_ != Running))
    {
      debug(SCHEDULER, "Scheduler::schedule: ERROR: no thread is in state Running!!");
      assert(false);
    }
  }
  while (currentThread->state_ != Running);
//   debug ( SCHEDULER,"Scheduler::schedule: new currentThread is %x %s, switch_userspace:%d\n",currentThread,currentThread->getName(),currentThread->switch_to_userspace_ );

  uint32 ret = 1;

  if ( currentThread->switch_to_userspace_ )
    currentThreadInfo =  currentThread->user_arch_thread_info_;
  else
  {
    currentThreadInfo =  currentThread->kernel_arch_thread_info_;
    ret=0;
  }

  return ret;
}

void Scheduler::yield()
{
  if ( ! ArchInterrupts::testIFSet() )
  {
    kprintfd ( "Scheduler::yield: WARNING Interrupts disabled, do you really want to yield ? (currentThread %x %s)\n", currentThread, currentThread->name_ );
    kprintf ( "Scheduler::yield: WARNING Interrupts disabled, do you really want to yield ?\n" );
  }
  ArchThreads::yield();
}

bool Scheduler::checkThreadExists ( Thread* thread )
{
  bool retval=false;
  lockScheduling();
  for ( uint32 c=0; c<threads_.size();++c ) //fortunately this doesn't involve KMM
    if ( threads_[c]==thread )
    {
      retval=true;
      break;
    }
  unlockScheduling();
  return retval;
}

void Scheduler::cleanupDeadThreads()
{
  //check outside of atmoarity for performance gain,
  //worst case, dead threads are around a while longer
  //then make sure we're atomar (can't really lock list, can I ;->)
  //NOTE: currentThread is always last on list

  if ( !kill_old_ )
    return;

  lockScheduling();
  waitForFreeKMMLock();
  //List<Thread*> destroy_list;
  ThreadList destroy_list;
  debug ( SCHEDULER,"cleanupDeadThreads: now running\n" );
  if ( kill_old_ )
  {
    //Thread *tmp_thread;
    for(ThreadList::iterator it=threads_.begin(); it!=threads_.end(); /* see in braces */)
    {
    	if((*it)->state_ == ToBeDestroyed)
    	{
    		destroy_list.push_back(*it);
    		it = threads_.erase(it);
    	}
    	else
    	{
    		++it;
    	}
    }

    /*for ( uint32 c=0; c< threads_.size(); ++c )
    {
      tmp_thread = threads_.front();
      if ( tmp_thread->state_ == ToBeDestroyed )
      {
        destroy_list.pushBack ( tmp_thread );
        threads_.popFront();
        --c;
        continue;
      }
      threads_.rotateBack();
    }*/
    kill_old_=false;
  }
  debug ( SCHEDULER, "cleanupDeadThreads: done\n" );
  unlockScheduling();
  /*while ( ! destroy_list.empty() )
  {
    Thread *cur_thread = destroy_list.front();
    destroy_list.popFront();
    delete cur_thread;
  }*/
  for(ThreadList::iterator it=destroy_list.begin(); it!=destroy_list.end(); ++it)
  {
	  delete *it;
  }
}

void Scheduler::printThreadList()
{
  const char *thread_states[6]= {"Running", "Sleeping", "ToBeDestroyed", "Unknown", "Unknown", "Unknown"};
  uint32 c=0;
  lockScheduling();
  debug ( SCHEDULER, "Scheduler::printThreadList: %d Threads in List\n",threads_.size() );
  for ( c=0; c<threads_.size();++c )
    debug ( SCHEDULER, "Scheduler::printThreadList: threads_[%d]: %x  %d:%s     [%s]\n",c,threads_[c],threads_[c]->getPID(),threads_[c]->getName(),thread_states[threads_[c]->state_] );
  unlockScheduling();
}

void Scheduler::lockScheduling()  //not as severe as stopping Interrupts
{
  if ( unlikely ( ArchThreads::testSetLock ( block_scheduling_,1 ) ) )
    arch_panic ( ( uint8* ) "FATAL ERROR: Scheduler::*: block_scheduling_ was set !! How the Hell did the program flow get here then ?\n" );
}
void Scheduler::unlockScheduling()
{
  block_scheduling_ = 0;
}
bool Scheduler::testLock()
{
  return ( block_scheduling_ > 0 );
}

void Scheduler::waitForFreeKMMLock()  //not as severe as stopping Interrupts
{
  if ( block_scheduling_==0 )
    arch_panic ( ( uint8* ) "FATAL ERROR: Scheduler::waitForFreeKMMLock: This "
                            "is meant to be used while Scheduler is locked\n" );

  while ( ! KernelMemoryManager::instance()->isKMMLockFree() )
  {
    unlockScheduling();
    yield();
    lockScheduling();
  }
}

void Scheduler::waitForFreeKMMLockAndFreeSpinLock(SpinLock &spinlock)
{
  if ( block_scheduling_==0 )
    arch_panic ( ( uint8* ) "FATAL ERROR: Scheduler::waitForFreeKMMLockAndFreeSpinLock"
                            ": This is meant to be used while Scheduler is locked\n" );

  while ( ! KernelMemoryManager::instance()->isKMMLockFree() ||
          ! spinlock.isFree())
  {
    unlockScheduling();
    yield();
    lockScheduling();
  }
}

void Scheduler::disableScheduling()
{
  lockScheduling();
  block_scheduling_extern_++;
  unlockScheduling();
}
void Scheduler::reenableScheduling()
{
  lockScheduling();
  if ( block_scheduling_extern_>0 )
    block_scheduling_extern_--;
  unlockScheduling();
}
bool Scheduler::isSchedulingEnabled()
{
  if ( this )
    return ( block_scheduling_==0 && block_scheduling_extern_==0 );
  else
    return false;
}

uint32 Scheduler::threadCount()
{
  return threads_.size();
}

uint32 Scheduler::getTicks()
{
  return ticks_;
}

void Scheduler::incTicks()
{
  ++ticks_;
}
