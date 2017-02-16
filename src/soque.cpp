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

static const uint32_t SOQUE_MAX_THREADS = std::thread::hardware_concurrency();

struct SOQUE
{
    enum
    {
        SOQUE_MARKER_EMPTY = 0,
        SOQUE_MARKER_PROCESSED = 1,
        SOQUE_MARKER_FILLED = 2,
    };

    void open( uint32_t size, void * arg, soque_push_cb push, soque_proc_cb proc, soque_pop_cb pop );
    uint32_t push( uint32_t push_count );
    SOQUE_BATCH proc_get( uint32_t batch );
    void proc_done( SOQUE_BATCH );
    uint32_t pop( uint32_t pop_count );
    uint8_t pp_enter();
    void pp_leave();
    void close();

    CACHELINE_ALIGN( std::atomic_bool soque_pp_guard );
    CACHELINE_ALIGN( uint32_t q_push );
    CACHELINE_ALIGN( std::atomic_uint32_t q_proc_run );
    CACHELINE_ALIGN( uint32_t q_proc );
    CACHELINE_ALIGN( uint32_t q_pop );
    CACHELINE_ALIGN( uint32_t q_size );
    void * cb_arg;
    soque_push_cb push_cb;
    soque_proc_cb proc_cb;
    soque_pop_cb pop_cb;
    void * original_alloc;
    CACHELINE_ALIGN( uint8_t markers[0] );
};

void SOQUE::open( uint32_t size, void * arg, soque_push_cb push, soque_proc_cb proc, soque_pop_cb pop )
{
    memset( this, 0, sizeof( SOQUE ) + sizeof( uint8_t ) * size );
    q_size = size;
    cb_arg = arg;
    push_cb = push;
    proc_cb = proc;
    pop_cb = pop;
}

void SOQUE::close()
{

}

uint8_t SOQUE::pp_enter()
{
    if( soque_pp_guard == false )
    {
        bool f = false;
        if( soque_pp_guard.compare_exchange_weak( f, true ) )
            return 1;
    }

    return 0;
}

void SOQUE::pp_leave()
{
    soque_pp_guard = false;
}

uint32_t SOQUE::push( uint32_t push_count )
{
    uint32_t push_here;
    uint32_t push_next;
    uint32_t push_max;

    push_here = q_push;
    push_max = q_pop;

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

    for( uint32_t i = 0; i < push_count; i++ )
    {
        if( push_here + i == q_size )
            push_here -= q_size;

        assert( markers[push_here + i] == SOQUE_MARKER_EMPTY );

        markers[push_here + i] = SOQUE_MARKER_FILLED;
    }

    q_push = push_next;

    return push_count;
}

SOQUE_BATCH SOQUE::proc_get( uint32_t proc_count )
{
    SOQUE_BATCH proc_batch;
    uint32_t proc_here;
    uint32_t proc_next;
    uint32_t proc_max;

    do
    {
        proc_here = q_proc_run;
        proc_max = q_push;

        if( proc_max == proc_here )
        {
            proc_batch.count = 0;
            return proc_batch;
        }

        if( proc_max > proc_here )
            proc_max = proc_max - proc_here;
        else
            proc_max = q_size + proc_max - proc_here;

        if( proc_count == 0 )
        {
            proc_batch.count = proc_max;
            return proc_batch;
        }

        if( proc_count > proc_max )
            proc_count = proc_max;

        proc_next = proc_here + proc_count;

        if( proc_next >= q_size )
            proc_next -= q_size;
    }
    while( !q_proc_run.compare_exchange_weak( proc_here, proc_next ) );

    proc_batch.index = proc_here;
    proc_batch.count = proc_count;
    
    for( uint32_t i = 0; i < proc_count; i++ )
    {
        if( proc_here + i == q_size )
            proc_here -= q_size;

        assert( markers[proc_here + i] == SOQUE_MARKER_FILLED );
    }

    return proc_batch;
}

void SOQUE::proc_done( SOQUE_BATCH proc_batch )
{
    uint32_t proc_here = proc_batch.index;

    for( uint32_t i = 0; i < proc_batch.count; i++ )
    {
        if( proc_here + i == q_size )
            proc_here -= q_size;

        markers[proc_here + i] = SOQUE_MARKER_PROCESSED;
    }
}

uint32_t SOQUE::pop( uint32_t pop_count )
{
    uint32_t pop_here;
    uint32_t pop_next;
    uint32_t pop_max;

    // finish q_proc
    {
        uint32_t proc_next = q_proc;
        uint32_t proc_run = q_proc_run;
        uint8_t isproc = 0;

        for( ;; )
        {
            if( proc_next == proc_run )
                break;

            if( markers[proc_next] != SOQUE_MARKER_PROCESSED )
                break;

            proc_next = proc_next + 1;

            if( proc_next == q_size )
                proc_next = 0;
            
            isproc = 1;
        }

        if( isproc )
            q_proc = proc_next;

        pop_max = proc_next;
    }    

    pop_here = q_pop;    

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

    for( uint32_t i = 0; i < pop_count; i++ )
    {
        if( pop_here + i >= q_size )
            pop_here -= q_size;

        assert( markers[pop_here + i] == SOQUE_MARKER_PROCESSED );

        markers[pop_here + i] = SOQUE_MARKER_EMPTY;
    }

    q_pop = pop_next;

    return pop_count;
}

SOQUE_HANDLE SOQUE_CALL soque_open( uint32_t size, void * cb_arg, soque_push_cb push_cb, soque_proc_cb proc_cb, soque_pop_cb pop_cb )
{
    void * mem = malloc( sizeof( SOQUE ) + sizeof( uint8_t ) * size + CACHELINE_SIZE );

    if( !mem )
        return NULL;

    SOQUE_HANDLE sh = CACHELINE_SHIFT( mem, SOQUE_HANDLE );
    sh->open( size, cb_arg, push_cb, proc_cb, pop_cb );
    sh->original_alloc = mem;

    return sh;
}

uint8_t SOQUE_CALL soque_pp_enter( SOQUE_HANDLE sh )
{
    return sh->pp_enter();
}

void SOQUE_CALL soque_pp_leave( SOQUE_HANDLE sh )
{
    sh->pp_leave();
}

uint32_t SOQUE_CALL soque_push( SOQUE_HANDLE sh, uint32_t push_count )
{
    return sh->push( push_count );
}

SOQUE_BATCH SOQUE_CALL soque_proc_get( SOQUE_HANDLE sh, uint32_t batch )
{
    return sh->proc_get( batch );
}

void SOQUE_CALL soque_proc_done( SOQUE_HANDLE sh, SOQUE_BATCH proc_batch )
{
    sh->proc_done( proc_batch );
}

uint32_t SOQUE_CALL soque_pop( SOQUE_HANDLE sh, uint32_t pop_count )
{
    return sh->pop( pop_count );
}

void SOQUE_CALL soque_close( SOQUE_HANDLE sh )
{
    void * mem = sh->original_alloc;
    sh->close();
    free( mem );
}

struct SOQUE_THREADS
{
    std::vector<SOQUE_HANDLE> soques_handles;
    uint8_t shutdown;
    uint32_t threads_count;
    uint32_t soques_count;
    uint32_t workers_count;
    uint32_t batch;
    uint32_t threshold;
    uint32_t reaction;
    std::atomic<uint32_t> threads_sync;
    std::vector<std::thread> threads;
    std::vector<uint32_t> speed_meter;

    void sit_on_cpu( std::thread & thread )
    {
        static uint32_t n = 0;

        if( n == SOQUE_MAX_THREADS )
            return;

#ifdef _WIN32

        SetThreadAffinityMask( thread.native_handle(), (DWORD_PTR)1 << n );

#elif !defined __CYGWIN__ 

        cpu_set_t cpuset;
        CPU_ZERO( &cpuset );
        CPU_SET( n, &cpuset );
        pthread_setaffinity_np( thread.native_handle(), sizeof( cpu_set_t ), &cpuset );

#endif

        n++;
    }

    uint8_t init( uint32_t t_count, uint8_t bind, SOQUE_HANDLE * sh, uint32_t sh_count )
    {
        memset( this, 0, sizeof( SOQUE_THREADS ) );
        soques_count = sh_count;
        threads_count = t_count == 0 ? SOQUE_MAX_THREADS : t_count;
        threads_sync = threads_count;
        batch = 16;
        threshold = 10000;
        reaction = 100;
        speed_meter.resize( threads_count );

        for( uint32_t i = 0; i < sh_count; i++ )
            soques_handles.push_back( sh[i] );

        for( uint32_t i = 0; i < threads_count; i++ )
            threads.push_back( std::thread( &soque_thread, this, i ) );

        threads.push_back( std::thread( &orchestra_thread, this ) );

        if( bind )
        {
            for( uint32_t i = 0; i < threads_count; i++ )
                sit_on_cpu( threads[i] );
        }

        return 1;
    }

    static void orchestra_thread( SOQUE_THREADS * sts )
    {
        uint32_t count = sts->threads_count;
        uint32_t i;
        std::vector<uint32_t> proc_meter_last;
        uint32_t workers_count;

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
                uint32_t speed_point = sts->speed_meter[i];
                uint32_t speed = (uint32_t)( ( speed_point - proc_meter_last[i] ) / time_span.count() );
                 
                if( speed > sts->threshold || ( workers_count == 0 && speed > sts->threshold / 100 ) )
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

    static void soque_thread( SOQUE_THREADS * sts, uint32_t thread_id )
    {
        uint32_t soques_count = sts->soques_count;
        SOQUE_HANDLE * soques_handles = &sts->soques_handles[0];
        uint32_t * proc_meter = &sts->speed_meter[thread_id];
        uint32_t proc_meter_cache = *proc_meter;
        uint32_t wake_point = thread_id < soques_count ? 0 : thread_id - soques_count + 1;

        sts->syncstart();

        for( uint32_t i = 0; sts->shutdown == 0; )
        {
            SOQUE_HANDLE sh = soques_handles[i];

            // PROC
            {
                SOQUE_BATCH proc_batch = soque_proc_get( sh, sts->batch );

                if( proc_batch.count )
                {
                    sh->proc_cb( sh->cb_arg, proc_batch );

                    soque_proc_done( sh, proc_batch );

                    proc_meter_cache += proc_batch.count;
                    *proc_meter = proc_meter_cache;
                }
            }

            // POP + PUSH
            if( soque_pp_enter( sh ) )
            {
                uint32_t queued;

                do
                {
                    // POP
                    {
                        queued = soque_pop( sh, 0 );

                        if( queued )
                        {
                            uint32_t popped = sh->pop_cb( sh->cb_arg, queued, sts->workers_count == 0 );

                            if( popped )
                                soque_pop( sh, popped );
                        }
                    }

                    // PUSH
                    {
                        uint32_t available = soque_push( sh, 0 );

                        if( available )
                        {
                            uint32_t pushed = sh->push_cb( sh->cb_arg, available, queued == 0 && sts->workers_count == 0 );

                            if( pushed )
                                soque_push( sh, pushed );
                        }
                    }
                }
                while( queued );

                soque_pp_leave( sh );
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

        for( uint32_t i = 0; i < threads_count; i++ )
            threads[i].join();
    }

    ~SOQUE_THREADS()
    {
        cleanup();
    }
};

SOQUE_THREADS_HANDLE SOQUE_CALL soque_threads_open( uint32_t threads_count, uint8_t bind, SOQUE_HANDLE * shs, uint32_t shs_count )
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

void SOQUE_CALL soque_threads_tune( SOQUE_THREADS_HANDLE sth, uint32_t batch, uint32_t threshold, uint32_t reaction )
{
    sth->batch = batch;
    sth->threshold = threshold;
    sth->reaction = reaction;
}

void SOQUE_CALL soque_threads_close( SOQUE_THREADS_HANDLE sth )
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
        soque_push,
        soque_proc_get,
        soque_proc_done,
        soque_pop,
        soque_pp_enter,
        soque_pp_leave,
        soque_close,
        soque_threads_open,
        soque_threads_tune,
        soque_threads_close,
    };

    return &soq;
}
