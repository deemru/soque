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

#define CACHELINE_SIZE 128
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

static const int SOQUE_MAX_THREADS = (int)std::thread::hardware_concurrency();

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
        guard_proc( SOQUE * soque, int priority )
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

    void open( int size, void * arg, soque_push_cb push, soque_proc_cb proc, soque_pop_cb pop );
    int push( int push_count );
    int proc_open( int proc_count, int * index );
    int proc_done( int proc_count, int index );
    int pop( int pop_count );
    void done();

    CACHELINE_ALIGN( int push_fixed );
    CACHELINE_ALIGN( int proc_run );
    CACHELINE_ALIGN( int proc_fixed );
    CACHELINE_ALIGN( int pop_fixed );
    CACHELINE_ALIGN( std::atomic_bool proc_lock );
    CACHELINE_ALIGN( int q_size );
    void * cb_arg;
    soque_push_cb push_cb;
    soque_proc_cb proc_cb;
    soque_pop_cb pop_cb;
    void * original_alloc;
    CACHELINE_ALIGN( MARKER markers[0] );
};

void SOQUE::open( int size, void * arg, soque_push_cb push, soque_proc_cb proc, soque_pop_cb pop )
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

int SOQUE::push( int push_count )
{
    int push_here;
    int push_next;
    int push_max;
    int pop_max;

    push_here = push_fixed;
    pop_max = pop_fixed - 1;

    if( pop_max < push_here )
        pop_max += q_size;

    if( pop_max == push_here )
        return 0;

    push_max = pop_max - push_here;

    if( push_count == 0 )
        return push_max;

    if( push_count > push_max )
        push_count = push_max; // надо бы считать _fixed

    push_next = push_here + push_count;

    if( push_next >= q_size )
        push_next -= q_size;

    for( int i = 0; i < push_count; i++ )
    {
        if( push_here + i == q_size )
            push_here -= q_size;

        assert( markers[push_here + i].status == SOQUE_MARKER_EMPTY );

        markers[push_here + i].status = SOQUE_MARKER_FILLED;
    }

    push_fixed = push_next;

    return push_count;
}

int SOQUE::proc_open( int proc_count, int * index )
{
    int proc_here;
    int proc_next;
    int proc_max;
    int push_max;

    {
        guard_proc gp( this, 0 );

        proc_here = proc_run;
        push_max = push_fixed;

        if( push_max < proc_here )
            push_max += q_size;

        if( push_max == proc_here )
            return 0;

        proc_max = push_max - proc_here;

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
    
    for( int i = 0; i < proc_count; i++ )
    {
        if( proc_here + i == q_size )
            proc_here -= q_size;

        assert( markers[proc_here + i].status == SOQUE_MARKER_FILLED );
    }

    return proc_count;
}


int SOQUE::proc_done( int proc_count, int proc_done )
{
    int proc_here = proc_done;
    int proc_next;

    {
        guard_proc gp( this, proc_done == proc_fixed );

        for( int i = 0; i < proc_count; i++ )
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

int SOQUE::pop( int pop_count )
{
    int pop_here;
    int pop_next;
    int pop_max;
    int proc_max;

    pop_here = pop_fixed;
    proc_max = proc_fixed;

    if( proc_max < pop_here )
        proc_max += q_size;

    if( proc_max == pop_here )
        return 0;

    pop_max = proc_max - pop_here;

    if( pop_count == 0 )
        return pop_max;

    if( pop_count > pop_max )
        pop_count = pop_max;

    pop_next = pop_here + pop_count;

    if( pop_next >= q_size )
        pop_next -= q_size;

    for( int i = 0; i < pop_count; i++ )
    {
        if( pop_here + i >= q_size )
            pop_here -= q_size;

        assert( markers[pop_here + i].status == SOQUE_MARKER_PROCESSED );

        markers[pop_here + i].status = SOQUE_MARKER_EMPTY;
    }

    pop_fixed = pop_next;

    return pop_count;
}

SOQUE_HANDLE SOQUE_CALL soque_open( int size, void * cb_arg, soque_push_cb push_cb, soque_proc_cb proc_cb, soque_pop_cb pop_cb )
{
    void * mem = malloc( sizeof( SOQUE ) + sizeof( SOQUE::MARKER ) * size + CACHELINE_SIZE );

    if( !mem )
        return NULL;

    SOQUE_HANDLE sh = CACHELINE_SHIFT( mem, SOQUE_HANDLE );
    sh->open( size, cb_arg, push_cb, proc_cb, pop_cb );
    sh->original_alloc = mem;

    return sh;
}

int SOQUE_CALL soque_push( SOQUE_HANDLE sh, int push_count )
{
    return sh->push( push_count );
}

int SOQUE_CALL soque_proc_open( SOQUE_HANDLE sh, int proc_count, int * proc_index )
{
    return sh->proc_open( proc_count, proc_index );
}

int SOQUE_CALL soque_proc_done( SOQUE_HANDLE sh, int proc_count, int proc_index )
{
    return sh->proc_done( proc_count, proc_index );
}

int SOQUE_CALL soque_pop( SOQUE_HANDLE sh, int pop_count )
{
    return sh->pop( pop_count );
}

void SOQUE_CALL soque_done( SOQUE_HANDLE sh )
{
    void * mem = sh->original_alloc;
    sh->done();
    free( mem );
}

struct SOQUE_THREAD
{
    CACHELINE_ALIGN( SOQUE_HANDLE sh );
    std::atomic_bool guard;
    int push;
    int pop;
    CACHELINE_ALIGN( char aligner[0] );
};



struct SOQUE_THREADS
{
    int shutdown;
    int threads_count;
    int soques_count;
    int wait;
    int wait_signal;
    void * mem;
    SOQUE_THREAD * t;
    std::vector<std::thread> vt;

    void sit_on_cpu( std::thread & thread )
    {
        static int n = 0;

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

    int init( int t_count, SOQUE_HANDLE * sh, int sh_count )
    {
        memset( this, 0, sizeof( SOQUE_THREADS ) );
        threads_count = t_count;
        soques_count = sh_count;
        wait = 0;
        wait_signal = 1;

        mem = malloc( sizeof( SOQUE_THREAD ) * sh_count + CACHELINE_SIZE );
        
        if( mem == NULL )
            return 0;

        t = CACHELINE_SHIFT( mem, SOQUE_THREAD * );

        for( int i = 0; i < sh_count; i++ )
        {
            memset( &t[i], 0, sizeof( SOQUE_THREAD ) );
            t[i].sh = sh[i];
        }

        for( int i = 0; i < threads_count; i++ )
            vt.push_back( std::thread( &soque_thread, this, i ) );

        vt.push_back( std::thread( &orchestra_thread, this ) );

        for( int i = 0; i < threads_count; i++ )
            sit_on_cpu( vt[i] );

        return 1;
    }

    static void orchestra_work( SOQUE_THREADS * sts )
    {
        sts->wait = 0;
        sts->wait_signal = 0;
    }

    static void orchestra_thread( SOQUE_THREADS * sts )
    {
        for( ; !sts->shutdown; )
        {
            sts->wait = 1;
            std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
            if( sts->wait )
                sts->wait_signal++;
        }
    }

    static void soque_thread( SOQUE_THREADS * sts, int thread_id )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

        int i = 0;
        int io_batch;
        int proc_batch;
        int proc_index;

        for( ; !sts->shutdown; )
        {
            SOQUE_HANDLE sh = sts->t[i].sh;

            if( thread_id >= sts->soques_count && sts->wait_signal > 16 )
                std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );

            // ... pop > push > proc ...
            if( i == thread_id )
            for( ;; )
            {
                io_batch = soque_pop( sh, 0 );

                if( io_batch )
                {
                    io_batch = sh->pop_cb( sh->cb_arg, io_batch, 0 );

                    if( io_batch )
                    {
                        soque_pop( sh, io_batch );

                        orchestra_work( sts );
                    }
                }

                io_batch = soque_push( sh, 0 );

                if( io_batch )
                {
                    io_batch = sh->push_cb( sh->cb_arg, io_batch, sts->wait_signal > 64 );

                    if( io_batch )
                    {
                        soque_push( sh, io_batch );

                        orchestra_work( sts );
                    }
                }

                for( ;; )
                {
                    if( 0 == ( proc_batch = soque_proc_open( sh, 16, &proc_index ) ) )
                        break;

                    sh->proc_cb( sh->cb_arg, proc_batch, proc_index );

                    soque_proc_done( sh, proc_batch, proc_index );
                    orchestra_work( sts );

                    if( proc_batch == 16 )
                        break;
                }

                if( io_batch )
                    continue;

                break;
            }
            else
            // proc only
            {
                for( ;; )
                {
                    if( 0 == ( proc_batch = soque_proc_open( sh, 64, &proc_index ) ) )
                        break;

                    sh->proc_cb( sh->cb_arg, proc_batch, proc_index );

                    soque_proc_done( sh, proc_batch, proc_index );
                    orchestra_work( sts );

                    if( proc_batch == 64 )
                        break;
                }
            }

            if( ++i == sts->soques_count )
                i = 0;
        }
    }

    void cleanup()
    {
        shutdown = 1;

        for( int i = 0; i < threads_count; i++ )
            vt[i].join();
    }

    ~SOQUE_THREADS()
    {
        cleanup();
    }
};

SOQUE_THREADS_HANDLE SOQUE_CALL soque_threads_open( int threads_count, SOQUE_HANDLE * shs, int shs_count )
{
    SOQUE_THREADS_HANDLE sth = (SOQUE_THREADS_HANDLE)malloc( sizeof( SOQUE_THREADS ) );

    if( !sth )
        return NULL;

    if( !sth->init( threads_count, shs, shs_count ) )
    {
        free( sth );
        return NULL;
    }

    return sth;
}

void SOQUE_CALL soque_threads_done( SOQUE_THREADS_HANDLE sth )
{
    sth->cleanup();
    free( sth );
}

