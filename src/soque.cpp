#include <atomic>
#include <thread>
#include <chrono>

#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#ifndef __CYGWIN__
#include <sched.h>
#include <pthread.h>
#endif
#endif

#include "soque.h"

#ifndef CACHELINE_SIZE
#define CACHELINE_SIZE 64
#endif
#define CACHELINE_SHIFT( addr, type ) ((type)( (uintptr_t)addr + CACHELINE_SIZE - ( (uintptr_t)addr % CACHELINE_SIZE ) ))

#ifdef _WIN32
#pragma warning( disable: 4200 ) // nonstandard extension used : zero-sized array in struct/union
#pragma warning( disable: 4324 ) // structure was padded due to __declspec(align())
#define CACHELINE_ALIGN( x ) __declspec( align( CACHELINE_SIZE ) ) x
#define rdtsc() __rdtsc()
#else
#define CACHELINE_ALIGN( x ) x __attribute__ ( ( aligned( CACHELINE_SIZE ) ) )
#define rdtsc() __builtin_ia32_rdtsc()
#endif

static const unsigned SOQUE_MAX_THREADS = std::thread::hardware_concurrency();

struct SOQUE
{
    enum
    {
        SOQUE_MARKER_EMPTY = 0,
        SOQUE_MARKER_PROCESSED,
        SOQUE_MARKER_FILLED,
    };

    struct MARKER
    {
        //CACHEALIGNED( char status );
        char status;
    };

    static void soque_yield()
    {
#if defined _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
    }

    struct guard_proc
    {
        guard_proc( SOQUE * soque, char priority )
        {
            for( ;; )
            {
                bool f = false;
                if( soque->proc_lock.compare_exchange_weak( f, true ) )
                    break;

                if( priority == 0 )
                    soque_yield();
            }

            soque_caller = soque;
        }

        ~guard_proc()
        {
            soque_caller->proc_lock = false;
        }

        SOQUE * soque_caller;
    };

    void open( unsigned size, void * arg, soque_push_cb push, soque_proc_cb proc, soque_pop_cb pop );
    char lock();
    unsigned push( unsigned push_count );
    unsigned proc_open( unsigned proc_count, unsigned * index );
    char proc_done( unsigned proc_count, unsigned index );
    unsigned pop( unsigned pop_count );
    void unlock();
    void done();

    CACHELINE_ALIGN( std::atomic_bool soque_lock );
    CACHELINE_ALIGN( std::atomic_bool proc_lock );
    CACHELINE_ALIGN( unsigned push_fixed );
    CACHELINE_ALIGN( unsigned proc_run );
    CACHELINE_ALIGN( unsigned proc_fixed );
    CACHELINE_ALIGN( unsigned pop_fixed );
    CACHELINE_ALIGN( unsigned q_size );
    void * cb_arg;
    soque_push_cb push_cb;
    soque_proc_cb proc_cb;
    soque_pop_cb pop_cb;
    void * original_alloc;
    CACHELINE_ALIGN( MARKER markers[0] );
};

void SOQUE::open( unsigned size, void * arg, soque_push_cb push, soque_proc_cb proc, soque_pop_cb pop )
{
    memset( this, 0, sizeof( SOQUE ) + sizeof( MARKER ) * size );
    q_size = size;
    cb_arg = arg;
    push_cb = push;
    proc_cb = proc;
    pop_cb = pop;
}

void SOQUE::done()
{

}

char SOQUE::lock()
{
    if( soque_lock == false )
    {
        bool f = false;
        if( soque_lock.compare_exchange_weak( f, true ) )
            return 1;
    }

    return 0;
}

void SOQUE::unlock()
{
    soque_lock = false;
}

unsigned SOQUE::push( unsigned push_count )
{
    unsigned push_here;
    unsigned push_next;
    unsigned push_max;

    push_here = push_fixed;
    push_max = pop_fixed;

    if( push_max > push_here )
        push_max = push_max - push_here - 1;
    else
        push_max = q_size + push_max - push_here - 1;

    if( push_max == 0 || push_count == 0 )
        return push_max;

    if( push_count > push_max )
        push_count = push_max;

    push_next = push_here + push_count;

    if( push_next >= q_size )
        push_next -= q_size;

    for( unsigned i = 0; i < push_count; i++ )
    {
        if( push_here + i == q_size )
            push_here -= q_size;

        assert( markers[push_here + i].status == SOQUE_MARKER_EMPTY );

        markers[push_here + i].status = SOQUE_MARKER_FILLED;
    }

    push_fixed = push_next;

    return push_count;
}

unsigned SOQUE::proc_open( unsigned proc_count, unsigned * index )
{
    unsigned proc_here;
    unsigned proc_next;
    unsigned proc_max;

    {
        guard_proc gp( this, 0 );

        proc_here = proc_run;
        proc_max = push_fixed;

        if( proc_max == proc_here )
            return 0;

        if( proc_max > proc_here )
            proc_max = proc_max - proc_here;
        else
            proc_max = q_size + proc_max - proc_here;

        if( proc_count == 0 )
            return proc_max;

        if( proc_count > proc_max )
            proc_count = proc_max;

        proc_next = proc_here + proc_count;

        if( proc_next >= q_size )
            proc_next -= q_size;

        proc_run = proc_next;
    }

    *index = proc_here;
    
    for( unsigned i = 0; i < proc_count; i++ )
    {
        if( proc_here + i == q_size )
            proc_here -= q_size;

        assert( markers[proc_here + i].status == SOQUE_MARKER_FILLED );
    }

    return proc_count;
}


char SOQUE::proc_done( unsigned proc_count, unsigned proc_done )
{
    unsigned proc_here = proc_done;
    unsigned proc_next;

    {
        guard_proc gp( this, proc_done == proc_fixed );

        for( unsigned i = 0; i < proc_count; i++ )
        {
            if( proc_here + i == q_size )
                proc_here -= q_size;

            markers[proc_here + i].status = SOQUE_MARKER_PROCESSED;
        }

        if( proc_done != proc_fixed )
            return 0;

        proc_next = proc_done + proc_count;

        if( proc_next >= q_size )
            proc_next -= q_size;

        for( ;; ) // +batch
        {
            if( proc_next == proc_run )
                break;

            if( markers[proc_next].status != SOQUE_MARKER_PROCESSED )
                break;

            proc_next = proc_next + 1;

            if( proc_next == q_size )
                proc_next = 0;
        }

        proc_fixed = proc_next;
    }    

    return 1;
}

unsigned SOQUE::pop( unsigned pop_count )
{
    unsigned pop_here;
    unsigned pop_next;
    unsigned pop_max;

    pop_here = pop_fixed;
    pop_max = proc_fixed;

    if( pop_max == pop_here )
        return 0;

    if( pop_max > pop_here )
        pop_max = pop_max - pop_here;
    else
        pop_max = q_size + pop_max - pop_here;

    if( pop_count == 0 )
        return pop_max;

    if( pop_count > pop_max )
        pop_count = pop_max;

    pop_next = pop_here + pop_count;

    if( pop_next >= q_size )
        pop_next -= q_size;

    for( unsigned i = 0; i < pop_count; i++ )
    {
        if( pop_here + i >= q_size )
            pop_here -= q_size;

        assert( markers[pop_here + i].status == SOQUE_MARKER_PROCESSED );

        markers[pop_here + i].status = SOQUE_MARKER_EMPTY;
    }

    pop_fixed = pop_next;

    return pop_count;
}

SOQUE_HANDLE SOQUE_CALL soque_open( unsigned size, void * cb_arg, soque_push_cb push_cb, soque_proc_cb proc_cb, soque_pop_cb pop_cb )
{
    void * mem = malloc( sizeof( SOQUE ) + sizeof( SOQUE::MARKER ) * size + CACHELINE_SIZE );

    if( !mem )
        return NULL;

    SOQUE_HANDLE sh = CACHELINE_SHIFT( mem, SOQUE_HANDLE );
    sh->open( size, cb_arg, push_cb, proc_cb, pop_cb );
    sh->original_alloc = mem;

    return sh;
}

char SOQUE_CALL soque_lock( SOQUE_HANDLE sh )
{
    return sh->lock();
}

void SOQUE_CALL soque_unlock( SOQUE_HANDLE sh )
{
    sh->unlock();
}

unsigned SOQUE_CALL soque_push( SOQUE_HANDLE sh, unsigned push_count )
{
    return sh->push( push_count );
}

unsigned SOQUE_CALL soque_proc_open( SOQUE_HANDLE sh, unsigned proc_count, unsigned * proc_index )
{
    return sh->proc_open( proc_count, proc_index );
}

char SOQUE_CALL soque_proc_done( SOQUE_HANDLE sh, unsigned proc_count, unsigned proc_index )
{
    return sh->proc_done( proc_count, proc_index );
}

unsigned SOQUE_CALL soque_pop( SOQUE_HANDLE sh, unsigned pop_count )
{
    return sh->pop( pop_count );
}

void SOQUE_CALL soque_done( SOQUE_HANDLE sh )
{
    void * mem = sh->original_alloc;
    sh->done();
    free( mem );
}

struct SOQUE_THREADS
{
    std::vector<SOQUE_HANDLE> soques_handles;
    char shutdown;
    unsigned threads_count;
    unsigned soques_count;
    unsigned workers_count;
    unsigned batch;
    unsigned threshold;
    unsigned reaction;
    std::atomic<unsigned> threads_sync;
    std::vector<std::thread> threads;
    std::vector<unsigned> speed_meter;

    void sit_on_cpu( std::thread & thread )
    {
        static unsigned n = 0;

        if( n == SOQUE_MAX_THREADS )
            return;

#ifdef _WIN32

        SetThreadAffinityMask( thread.native_handle(), 1 << n );

#elif !defined __CYGWIN__ 

        cpu_set_t cpuset;
        CPU_ZERO( &cpuset );
        CPU_SET( n, &cpuset );
        pthread_setaffinity_np( thread.native_handle(), sizeof( cpu_set_t ), &cpuset );

#endif

        n++;
    }

    char init( unsigned t_count, char bind, SOQUE_HANDLE * sh, unsigned sh_count )
    {
        memset( this, 0, sizeof( SOQUE_THREADS ) );
        soques_count = sh_count;
        threads_count = t_count == 0 ? SOQUE_MAX_THREADS : t_count;
        threads_sync = threads_count;
        batch = 16;
        threshold = 10000;
        reaction = 100;
        speed_meter.resize( threads_count );

        for( unsigned i = 0; i < sh_count; i++ )
            soques_handles.push_back( sh[i] );

        for( unsigned i = 0; i < threads_count; i++ )
            threads.push_back( std::thread( &soque_thread, this, i ) );

        threads.push_back( std::thread( &orchestra_thread, this ) );

        if( bind )
        {
            for( unsigned i = 0; i < threads_count; i++ )
                sit_on_cpu( threads[i] );
        }

        return 1;
    }

    static void orchestra_thread( SOQUE_THREADS * sts )
    {
        unsigned count = sts->threads_count;
        unsigned i;
        std::vector<unsigned> proc_meter_last;
        unsigned workers_count;

        proc_meter_last.resize( count );        
        std::chrono::high_resolution_clock::time_point time_last = std::chrono::high_resolution_clock::now();

        for( ; !sts->shutdown; )
        {
            std::this_thread::sleep_for( std::chrono::milliseconds( sts->reaction ) );

            std::chrono::high_resolution_clock::time_point time_now = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> time_span = std::chrono::duration_cast<std::chrono::duration<double>>( time_now - time_last );

            time_last = time_now;
            workers_count = 0;

            for( i = 0; i < count; i++ )
            {
                unsigned speed_point = sts->speed_meter[i];
                unsigned speed = (unsigned)( ( speed_point - proc_meter_last[i] ) / time_span.count() );
                 
                if( speed > sts->threshold )
                    workers_count++;

                proc_meter_last[i] = speed_point;
            }

            sts->workers_count = workers_count;
        }
    }

    void syncstart()
    {
        threads_sync--;
        while( threads_sync != 0 )
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
    }

    static void soque_thread( SOQUE_THREADS * sts, unsigned thread_id )
    {
        unsigned io_batch;
        unsigned proc_batch;
        unsigned proc_index;
        unsigned soques_count = sts->soques_count;
        SOQUE_HANDLE * soques_handles = &sts->soques_handles[0];
        unsigned * proc_meter = &sts->speed_meter[thread_id];
        unsigned proc_meter_cache = *proc_meter;
        unsigned wake_point = thread_id < soques_count ? 0 : thread_id - soques_count + 1;
        SOQUE_HANDLE sh;

        sts->syncstart();

        for( unsigned i = 0; sts->shutdown == 0; )
        {
            sh = soques_handles[i];

            // proc
            {
                proc_batch = soque_proc_open( sh, sts->batch, &proc_index );

                if( proc_batch )
                {
                    sh->proc_cb( sh->cb_arg, proc_batch, proc_index );

                    soque_proc_done( sh, proc_batch, proc_index );

                    proc_meter_cache += proc_batch;
                    *proc_meter = proc_meter_cache;
                }
            }

            // pop > push
            if( soque_lock( sh ) )
            {
                io_batch = soque_pop( sh, 0 );

                if( io_batch )
                {
                    io_batch = sh->pop_cb( sh->cb_arg, io_batch, sts->workers_count == 0 );

                    if( io_batch )
                        soque_pop( sh, io_batch );
                }

                io_batch = soque_push( sh, 0 );

                if( io_batch )
                {
                    io_batch = sh->push_cb( sh->cb_arg, io_batch, sts->workers_count == 0 );

                    if( io_batch )
                        soque_push( sh, io_batch );
                }

                soque_unlock( sh );
            }

            if( ++i == soques_count )
            {
                i = 0;

                if( wake_point && sts->workers_count < wake_point )
                    std::this_thread::sleep_for( std::chrono::milliseconds( sts->reaction ) );
            }
        }
    }

    void cleanup()
    {
        shutdown = 1;

        for( unsigned i = 0; i < threads_count; i++ )
            threads[i].join();
    }

    ~SOQUE_THREADS()
    {
        cleanup();
    }
};

SOQUE_THREADS_HANDLE SOQUE_CALL soque_threads_open( unsigned threads_count, char bind, SOQUE_HANDLE * shs, unsigned shs_count )
{
    SOQUE_THREADS_HANDLE sth = (SOQUE_THREADS_HANDLE)malloc( sizeof( SOQUE_THREADS ) );

    if( !sth )
        return NULL;

    if( !sth->init( threads_count, bind, shs, shs_count ) )
    {
        free( sth );
        return NULL;
    }

    return sth;
}

void SOQUE_CALL soque_threads_tune( SOQUE_THREADS_HANDLE sth, unsigned batch, unsigned threshold, unsigned reaction )
{
    sth->batch = batch;
    sth->threshold = threshold;
    sth->reaction = reaction;
}

void SOQUE_CALL soque_threads_done( SOQUE_THREADS_HANDLE sth )
{
    sth->cleanup();
    free( sth );
}

const SOQUE_FRAMEWORK * soque_framework()
{
    static const SOQUE_FRAMEWORK soq = {
        SOQUE_MAJOR,
        SOQUE_MINOR,
        soque_open,
        soque_lock,
        soque_push,
        soque_proc_open,
        soque_proc_done,
        soque_pop,
        soque_unlock,
        soque_done,
        soque_threads_open,
        soque_threads_tune,
        soque_threads_done,
    };

    return &soq;
}
