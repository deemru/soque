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

#if defined _WIN32

#pragma warning( disable: 4200 ) // nonstandard extension used : zero-sized array in struct/union
#pragma warning( disable: 4324 ) // structure was padded due to __declspec(align())
#define CACHELINE_ALIGN( x ) __declspec( align( CACHELINE_SIZE ) ) x
#define rdtsc() __rdtsc()

#else

#define CACHELINE_ALIGN( x ) x __attribute__ ( ( aligned( CACHELINE_SIZE ) ) )
#define rdtsc() __builtin_ia32_rdtsc()

#endif

#define SOQUE_DEFAULT_PENALTY 100000
#define SOQUE_DEFAULT_Q_SIZE 100000

static const int SOQUE_MAX_THREADS = (int)std::thread::hardware_concurrency();
static unsigned SOQUE_PENALTY = SOQUE_DEFAULT_PENALTY;
static std::atomic<long long> g_read_count;

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

    static void penalty_yield()
    {
#if 1
#if defined _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
#else
        std::this_thread::sleep_for( std::chrono::nanoseconds( 1 ) );
#endif
    }

    static void penalty_rdtsc()
    {
        return;
        //unsigned long long now = rdtsc();
        //while( rdtsc() - now < SOQUE_PENALTY ){}
    }

    struct guard_proc
    {
        guard_proc( SOQUE * soque )
        {
            if( soque->proc_threads > 1 )
                for( ;; )
                {
                    bool f = false;
                    if( soque->proc_lock.compare_exchange_weak( f, true ) )
                        break;
                    soque->proc_penalty();
                }

            soque_caller = soque;
        }

        ~guard_proc()
        {
            if( soque_caller->proc_threads > 1 )
                soque_caller->proc_lock = false;
        }

        SOQUE * soque_caller;
    };

    void open( int size,
        int threads,
        void * arg,
        soque_push_cb push,
        soque_proc_cb proc,
        soque_pop_cb pop );
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
    int proc_threads;
    void ( * proc_penalty )();

    void * cb_arg;
    soque_push_cb push_cb;
    soque_proc_cb proc_cb;
    soque_pop_cb pop_cb;

    void * mem;

    CACHELINE_ALIGN( MARKER markers[0] );
};

void SOQUE::open( int size,
                  int threads,
                  void * arg,
                  soque_push_cb push,
                  soque_proc_cb proc,
                  soque_pop_cb pop )
{
    memset( this, 0, sizeof( SOQUE ) + sizeof( MARKER ) * size );
    q_size = size;
    proc_threads = threads;
    proc_penalty = proc_threads > (int)SOQUE_MAX_THREADS ? penalty_yield : penalty_rdtsc;

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
        guard_proc gp( this );

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
        guard_proc gp( this );

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

SOQUE_HANDLE SOQUE_CALL soque_open( int queue_size,
                         int max_threads,
                         void * cb_arg,
                         soque_push_cb push_cb,
                         soque_proc_cb proc_cb,
                         soque_pop_cb pop_cb )
{
    void * mem = malloc( sizeof( SOQUE ) + sizeof( SOQUE::MARKER ) * queue_size + CACHELINE_SIZE );

    if( !mem )
        return NULL;

    SOQUE_HANDLE sh = CACHELINE_SHIFT( mem, SOQUE_HANDLE );
    sh->open( queue_size, max_threads, cb_arg, push_cb, proc_cb, pop_cb );
    sh->mem = mem;

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
    void * mem = sh->mem;
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

        for( int i = 0; i < threads_count; i++ )
            sit_on_cpu( vt[i] );

        return 1;
    }

    static void soque_thread( SOQUE_THREADS * sts, int thread_id )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

        int i = 0;
        int pos;
        int batch;
        int batch_check;
        int nowork = 1;
        int max_batch = 16;

        for( ; !sts->shutdown; )
        {
            SOQUE_HANDLE sh = sts->t[i].sh;

            // ... pop > push > proc ...
            if( i == thread_id )
            for( ;; )
            {
                batch = soque_pop( sh, 0 );

                if( batch )
                {
                    batch = sh->pop_cb( sh->cb_arg, batch, 0 );

                    if( batch )
                    {
                        soque_pop( sh, batch );

                        g_read_count += batch;
                    }

                    nowork = 0;
                }

                batch = soque_push( sh, 0 );

                if( batch )
                {
                    batch = sh->push_cb( sh->cb_arg, batch, nowork > 8 );

                    if( batch )
                    {
                        batch_check = soque_push( sh, batch );

                        if( batch_check != batch )
                        {
                            batch_check += soque_push( sh, batch - batch_check );
                            assert( batch_check == batch );
                        }

                        nowork = 0;
                    }
                }

                batch = 0;

                for( ;; )
                {
                    if( 0 == ( batch = soque_proc_open( sh, max_batch, &pos ) ) )
                        break;

                    sh->proc_cb( sh->cb_arg, batch, pos );

                    soque_proc_done( sh, batch, pos );
                    nowork = 0;

                    if( batch == max_batch )
                        break;
                }

                if( batch )
                    continue;

                break;
            }
            else
            // proc only
            {
                batch = 0;

                for( ;; )
                {
                    if( 0 == ( batch = soque_proc_open( sh, max_batch, &pos ) ) )
                        break;

                    sh->proc_cb( sh->cb_arg, batch, pos );

                    soque_proc_done( sh, batch, pos );
                    nowork = 0;

                    if( batch == max_batch )
                        break;
                }
            }

            if( ++i == sts->soques_count )
            {
                i = 0;
                if( ++nowork > 8 )
                {
                    std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
                }
            }
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

static int SOQUE_CALL empty_soque_cb( void * arg, int count, int waitable )
{
    (void)arg;
    (void)waitable;

    return count;
}


static void SOQUE_CALL empty_soque_proc_cb( void * arg, int pos, int count )
{
    (void)arg;
    (void)pos;
    (void)count;
}

static soque_push_cb push_cb = &empty_soque_cb;
static soque_proc_cb proc_cb = &empty_soque_proc_cb;
static soque_pop_cb pop_cb = &empty_soque_cb;

int main( int argc, char ** argv )
{
    int queue_size = SOQUE_DEFAULT_Q_SIZE;
    int threads_count = 1;

    if( argc == 4 )
    {
        queue_size = atoi( argv[1] );
        threads_count = atoi( argv[2] );
        SOQUE_PENALTY = atoi( argv[3] );
    }

    printf( "queue_size = %d\n", queue_size );
    printf( "threads_count = %d\n", threads_count );
    printf( "SOQUE_PENALTY = %d\n\n", SOQUE_PENALTY );
    
#if 0

    SOQUE_HANDLE q[3];
    q[2] = soque_open( q_count, 1, NULL, NULL, NULL, NULL );
    q[0] = soque_open( q_count, p_count, q[2], push_cb, proc_cb, (soque_pop_batch_cb)&soque_push_batch );
    q[1] = soque_open( q_count, p_count, q[2], (soque_push_batch_cb)&soque_pop_batch, proc_cb, pop_cb );

    std::thread temp( [&]() {
        int pos;

        for( ;; )
        {
            while( ( pos = q[2]->proc_get() ) != -1 )
            {
                //std::this_thread::sleep_for( std::chrono::milliseconds( 1 ) );
                q[2]->proc_sync( pos );
            }

            //std::this_thread::sleep_for( std::chrono::milliseconds( 1000 ) );
        }
    } );
#else

    SOQUE_HANDLE q[2];
    q[0] = soque_open( queue_size, threads_count, NULL, push_cb, proc_cb, pop_cb );
    q[1] = soque_open( queue_size, threads_count, NULL, push_cb, proc_cb, pop_cb );

#endif

    

    printf( "sizeof( struct SOQUE ) = %d\n", ( int )sizeof( SOQUE ) );
    printf( "sizeof( struct SOQUE_THREAD ) = %d\n", ( int )sizeof( SOQUE_THREAD ) );
    printf( "sizeof( struct SOQUE_THREADS ) = %d\n\n", ( int )sizeof( SOQUE_THREADS ) );


    SOQUE_THREADS_HANDLE sth;
    sth = soque_threads_open( threads_count, q, 2 );

    std::this_thread::sleep_for( std::chrono::milliseconds( 1000 ) ); // warming

    double speed_moment = 0;
    double speed_approx = 0;
    int n = 0;

    for( ;; )
    {
        long long speed_save = g_read_count;
        std::this_thread::sleep_for( std::chrono::milliseconds( 1000 ) );
        double speed_change = speed_moment;
        double speed_approx_change = speed_approx;
        speed_moment = (double)( g_read_count - speed_save );
        speed_approx = ( speed_approx * n + speed_moment ) / ( n + 1 );
        printf( "Mpps:   %.03f (%s%0.03f)   ~   %.03f (%s%0.03f)\n", 
            speed_moment / 1000000,
            speed_change <= speed_moment ? "+" : "", ( speed_moment - speed_change ) / 1000000,
            speed_approx / 1000000,
            speed_approx_change <= speed_approx ? "+" : "", ( speed_approx - speed_approx_change ) / 1000000 );
        n++;
    }
}
