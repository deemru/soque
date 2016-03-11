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
        unsigned long long now = rdtsc();
        while( rdtsc() - now < SOQUE_PENALTY ){}
    }

    struct guard_proc
    {
        guard_proc( SOQUE * soque )
        {
            if( soque->ps > 1 )
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
            if( soque_caller->ps > 1 )
                soque_caller->proc_lock = false;
        }

        SOQUE * soque_caller;
    };

    void init( int size,
        int threads,
        void * arg,
        soque_push_batch_cb push,
        soque_proc_batch_cb proc,
        soque_pop_batch_cb pop );
    int push_batch( int push_count );
    int proc_get();
    int proc_sync( int proc_done );
    int pop_batch( int pop_count );
    void cleanup();

#if 0
    CACHELINE_ALIGN( int push_fixed );
    int pop_cached;
    CACHELINE_ALIGN( int proc_run );
    int push_cached;
    CACHELINE_ALIGN( int proc_fixed );
    CACHELINE_ALIGN( int pop_fixed );
    int proc_cached;
#else
    CACHELINE_ALIGN( int push_fixed );
    CACHELINE_ALIGN( int pop_cached );
    CACHELINE_ALIGN( int proc_run );
    CACHELINE_ALIGN( int push_cached );
    CACHELINE_ALIGN( int proc_fixed );
    CACHELINE_ALIGN( int pop_fixed );
    CACHELINE_ALIGN( int proc_cached );
#endif

    CACHELINE_ALIGN( std::atomic_bool proc_lock );

    CACHELINE_ALIGN( int q_size );
    int ps;
    void ( * push_penalty )();
    void ( * proc_penalty )();
    void ( * pop_penalty )();

    void * cb_arg;
    soque_push_batch_cb push_cb;
    soque_proc_batch_cb proc_cb;
    soque_pop_batch_cb pop_cb;

    void * mem;

    CACHELINE_ALIGN( MARKER markers[0] );
};

void SOQUE::init( int size,
                  int threads,
                  void * arg,
                  soque_push_batch_cb push,
                  soque_proc_batch_cb proc,
                  soque_pop_batch_cb pop )
{
    memset( this, 0, sizeof( SOQUE ) + sizeof( MARKER ) * size );
    q_size = size;
    ps = threads;
    push_penalty = penalty_rdtsc;
    proc_penalty = ps > (int)SOQUE_MAX_THREADS ? penalty_yield : penalty_rdtsc;
    pop_penalty = penalty_rdtsc;

    cb_arg = arg;
    push_cb = push;
    proc_cb = proc;
    pop_cb = pop;
}

void SOQUE::cleanup()
{

}

int SOQUE::proc_get()
{
    int proc_here;
    int proc_next;

    for( ;; )
    {
        {
            guard_proc gp( this );

            proc_here = proc_run;

            if( proc_here == push_cached )
            {
                if( proc_here == push_fixed )
                    break;

                push_cached = push_fixed;
            }

            proc_next = proc_here + 1;

            if( proc_next == q_size )
                proc_next = 0;

            proc_run = proc_next;
        }

        assert( markers[proc_here].status == SOQUE_MARKER_FILLED );
        return proc_here;
    }

    proc_penalty();
    return -1;
}

int SOQUE::proc_sync( int proc_done )
{
    int proc_next;

    guard_proc gp( this );

    markers[proc_done].status = SOQUE_MARKER_PROCESSED;

    if( proc_done != proc_fixed )
        return 0;

    proc_next = proc_done + 1;

    for( ;; )
    {
        if( proc_next == q_size )
            proc_next = 0;

        if( proc_next == proc_run )
            break;

        if( markers[proc_next].status != SOQUE_MARKER_PROCESSED )
            break;

        proc_next = proc_next + 1;
    }

    proc_fixed = proc_next;

    return 1;
}

int SOQUE::push_batch( int push_count )
{
    int push_here;
    int push_next;
    int push_max;
    int pop_max;

    push_here = push_fixed;
    pop_max = pop_cached - 1;

    if( pop_max < push_here )
        pop_max += q_size;

    if( pop_max == push_here )
    {
        pop_cached = pop_fixed;

        pop_max = pop_cached - 1;

        if( pop_max < push_here )
            pop_max += q_size;

        if( pop_max == push_here )
            return 0;
    }

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
        if( push_here + i >= q_size )
            push_here -= q_size;

        assert( markers[push_here + i].status == SOQUE_MARKER_EMPTY );

        markers[push_here + i].status = SOQUE_MARKER_FILLED;
    }

    push_fixed = push_next;

    return push_count;
}

int SOQUE::pop_batch( int pop_count )
{
    int pop_here;
    int pop_next;
    int pop_max;
    int proc_max;

    pop_here = pop_fixed;
    proc_max = proc_cached;

    if( proc_max < pop_here )
        proc_max += q_size;

    if( proc_max == pop_here )
    {
        proc_cached = proc_fixed;

        proc_max = proc_cached;

        if( proc_max < pop_here )
            proc_max += q_size;

        if( proc_max == pop_here )
            return 0;
    }

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
                         soque_push_batch_cb push_cb,
                         soque_proc_batch_cb proc_cb,
                         soque_pop_batch_cb pop_cb )
{
    void * mem = malloc( sizeof( SOQUE ) + sizeof( SOQUE::MARKER ) * queue_size + CACHELINE_SIZE );

    if( !mem )
        return NULL;

    SOQUE_HANDLE sh = CACHELINE_SHIFT( mem, SOQUE_HANDLE );
    sh->init( queue_size, max_threads, cb_arg, push_cb, proc_cb, pop_cb );
    sh->mem = mem;

    return sh;
}

int SOQUE_CALL soque_push_batch( SOQUE_HANDLE sh, int push_count )
{
    return sh->push_batch( push_count );
}

int SOQUE_CALL soque_proc_get( SOQUE_HANDLE sh )
{
    return sh->proc_get();
}

int SOQUE_CALL soque_proc_sync( SOQUE_HANDLE sh, int proc_done )
{
    return sh->proc_sync( proc_done );
}

int SOQUE_CALL soque_pop_batch( SOQUE_HANDLE sh, int pop_count )
{
    return sh->pop_batch( pop_count );
}

void SOQUE_CALL soque_close( SOQUE_HANDLE sh )
{
    void * mem = sh->mem;
    sh->cleanup();
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
            vt.push_back( std::thread( &soque_thread, this ) );

        for( int i = 0; i < threads_count; i++ )
            sit_on_cpu( vt[i] );

        return 1;
    }

    static void soque_thread( SOQUE_THREADS * sts )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

        int i = 0;
        int batch;
        int batch_check;

        for( ; !sts->shutdown; )
        {
            SOQUE_HANDLE sh = sts->t[i].sh;

            // proc
            {
                int pos;
                int batch = 0;

                for( ;; )
                {
                    if( ( pos = soque_proc_get( sh ) ) == -1 )
                        break;

                    sh->proc_cb( sh->cb_arg, pos, 1 );

                    soque_proc_sync( sh, pos );

                    if( ++batch == 256 )
                        break;
                }
            }

            // pop + push
            {
                bool f = false;

                if( sts->t[i].guard.compare_exchange_weak( f, true ) )
                {
                    batch = soque_pop_batch( sh, 0 );
                    
                    if( batch )
                    {
                        batch = sh->pop_cb( sh->cb_arg, batch );

                        if( batch )
                        {
                            soque_pop_batch( sh, batch );

                            g_read_count += batch;
                        }
                    }

                    batch = soque_push_batch( sh, 0 );

                    if( batch )
                    {
                        batch = sh->push_cb( sh->cb_arg, batch );

                        if( batch )
                        {
                            batch_check = soque_push_batch( sh, batch );

                            if( batch_check != batch )
                            {
                                batch_check += soque_push_batch( sh, batch - batch_check );
                                assert( batch_check == batch );
                            }
                        }
                    }

                    sts->t[i].guard = false;
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

void SOQUE_CALL soque_threads_close( SOQUE_THREADS_HANDLE sth )
{
    sth->cleanup();
    free( sth );
}

static int SOQUE_CALL soque_cb( void * arg, int count )
{
    (void)arg;

    return count;
}


static void SOQUE_CALL soque_proc_cb( void * arg, int pos, int count )
{
    (void)arg;
    (void)pos;
    (void)count;
}

static soque_push_batch_cb push_cb = &soque_cb;
static soque_proc_batch_cb proc_cb = &soque_proc_cb;
static soque_pop_batch_cb pop_cb = &soque_cb;

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
