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

    struct guard_read
    {
        guard_read( SOQUE * soque )
        {
            if( soque->rs > 1 )
                for( ;; )
                {
                    bool f = false;
                    if( soque->r_lock.compare_exchange_weak( f, true ) )
                        break;
                    soque->r_penalty();
                }

            soque_caller = soque;
        }

        ~guard_read()
        {
            if( soque_caller->rs > 1 )
                soque_caller->r_lock = false;
        }

        SOQUE * soque_caller;
    };

    struct guard_write
    {
        guard_write( SOQUE * soque )
        {
            if( soque->ws > 1 )
                for( ;; )
                {
                    bool f = false;
                    if( soque->w_lock.compare_exchange_weak( f, true ) )
                        break;
                    soque->w_penalty();
                }

            soque_caller = soque;
        }

        ~guard_write()
        {
            if( soque_caller->ws > 1 )
                soque_caller->w_lock = false;
        }

        SOQUE * soque_caller;
    };

    struct guard_proc
    {
        guard_proc( SOQUE * soque )
        {
            if( soque->ps > 1 )
                for( ;; )
                {
                    bool f = false;
                    if( soque->p_lock.compare_exchange_weak( f, true ) )
                        break;
                    soque->p_penalty();
                }

            soque_caller = soque;
        }

        ~guard_proc()
        {
            if( soque_caller->ps > 1 )
                soque_caller->p_lock = false;
        }

        SOQUE * soque_caller;
    };

    void init( int size,
        int writers,
        int procers,
        int readers,
        void * arg,
        soque_push_batch_cb push,
        soque_push_batch_cb proc,
        soque_push_batch_cb pop );
    int write_get();
    int write_sync( int write_done );
    int proc_get();
    int proc_sync( int proc_done );
    int read_get();
    int read_sync( int read_done );
    int push_batch( int push_count );
    int pop_batch( int pop_count );
    void cleanup();

    CACHELINE_ALIGN( volatile int w_pos );
    CACHELINE_ALIGN( volatile int w_sync );
    CACHELINE_ALIGN( volatile int w_ready );
    CACHELINE_ALIGN( volatile int p_pos );
    CACHELINE_ALIGN( volatile int p_sync );
    CACHELINE_ALIGN( volatile int p_ready );
    CACHELINE_ALIGN( volatile int r_pos );
    CACHELINE_ALIGN( volatile int r_sync );
    CACHELINE_ALIGN( volatile int r_ready );
    CACHELINE_ALIGN( std::atomic_bool w_lock );
    CACHELINE_ALIGN( std::atomic_bool p_lock );
    CACHELINE_ALIGN( std::atomic_bool r_lock );

    CACHELINE_ALIGN( int q_size );
    int ws;
    int ps;
    int rs;
    void ( * w_penalty )();
    void ( * p_penalty )();
    void ( * r_penalty )();

    void * cb_arg;
    soque_push_batch_cb push_cb;
    soque_push_batch_cb proc_cb;
    soque_push_batch_cb pop_cb;

    void * mem;

    CACHELINE_ALIGN( MARKER q[0] );
};

void SOQUE::init( int size,
                  int writers,
                  int procers,
                  int readers,
                  void * arg,
                  soque_push_batch_cb push,
                  soque_push_batch_cb proc,
                  soque_push_batch_cb pop )
{
    memset( this, 0, sizeof( SOQUE ) + sizeof( MARKER ) * size );
    q_size = size;
    ws = writers;
    ps = procers;
    rs = readers;
    w_penalty = penalty_rdtsc;
    p_penalty = ws + ps + rs > (int)SOQUE_MAX_THREADS ? penalty_yield : penalty_rdtsc;
    r_penalty = penalty_rdtsc;

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
    int p_here;
    int p_next;

    for( ;; )
    {
        {
            guard_proc gp( this );

            p_here = p_pos;

            if( p_here == w_ready )
            {
                if( p_here == w_sync )
                    break;

                w_ready = w_sync;
            }

            p_next = p_here + 1;

            if( p_next == q_size )
                p_next = 0;

            p_pos = p_next;
        }

        assert( q[p_here].status == SOQUE_MARKER_FILLED );
        return p_here;
    }

    p_penalty();
    return -1;
}

int SOQUE::proc_sync( int p_done )
{
    int p_next;

    guard_proc gp( this );

    q[p_done].status = SOQUE_MARKER_PROCESSED;

    if( p_done != p_sync )
        return 0;

    p_next = p_done + 1;

    for( ;; )
    {
        if( p_next == q_size )
            p_next = 0;

        if( p_next == p_pos )
            break;

        if( q[p_next].status != SOQUE_MARKER_PROCESSED )
            break;

        p_next = p_next + 1;
    }

    p_sync = p_next;

    return 1;
}

int SOQUE::read_get()
{
    int r_here;
    int r_next;

    for( ;; )
    {
        {
            guard_read gr( this );

            r_here = r_pos;

            if( r_here == p_ready )
            {
                if( r_here == p_sync )
                    break;

                p_ready = p_sync;
            }

            r_next = r_here + 1;

            if( r_next == q_size )
                r_next = 0;

            r_pos = r_next;
        }

        assert( q[r_here].status == SOQUE_MARKER_PROCESSED );
        return r_here;
    }

    r_penalty();
    return -1;
}

int SOQUE::read_sync( int r_done )
{
    int r_next;

    guard_read gr( this );

    q[r_done].status = SOQUE_MARKER_EMPTY;

    if( r_done != r_sync )
        return 0;

    r_next = r_done + 1;

    for( ;; )
    {
        if( r_next == q_size )
            r_next = 0;

        if( r_next == r_pos )
            break;

        if( q[r_next].status != SOQUE_MARKER_EMPTY )
            break;

        r_next = r_next + 1;
    }

    r_sync = r_next;

    return 1;
}

int SOQUE::write_get()
{
    int w_here;
    int w_next;

    for( ;; )
    {
        {
            guard_write gw( this );

            w_here = w_pos;
            w_next = w_here + 1;

            if( w_next == q_size )
                w_next = 0;

            if( w_next == r_ready )
            {
                if( w_next == r_sync )
                    break;

                r_ready = r_sync;
            }

            w_pos = w_next;
        }

        assert( q[w_here].status == SOQUE_MARKER_EMPTY );
        return w_here;
    }

    w_penalty();
    return -1;
}

int SOQUE::push_batch( int push_count )
{
    int w_here;
    int w_next;
    int push_max;
    int r_max;

    guard_write gw( this );

    w_here = w_pos;
    r_max = r_ready - 1;

    if( r_max < w_here )
        r_max += q_size;

    if( r_max == w_here )
    {
        r_ready = r_sync;

        r_max = r_ready - 1;

        if( r_max < w_here )
            r_max += q_size;

        if( r_max == w_here )
        {
            //w_penalty();
            return 0;
        }
    }

    push_max = r_max - w_here;

    if( push_count == 0 )
        return push_max;

    if( push_count > push_max )
        push_count = push_max;

    w_next = w_here + push_count;

    if( w_next >= q_size )
        w_next -= q_size;

    w_pos = w_next;

    for( int i = 0; i < push_count; i++ )
    {
        if( w_here + i >= q_size )
            w_here -= q_size;

        assert( q[w_here + i].status == SOQUE_MARKER_EMPTY );

        q[w_here + i].status = SOQUE_MARKER_FILLED;
    }

    w_sync = w_next;

    return push_count;
}

int SOQUE::pop_batch( int pop_count )
{
    int r_here;
    int r_next;
    int pop_max;
    int p_max;

    guard_read gr( this );

    r_here = r_pos;
    p_max = p_ready;

    if( p_max < r_here )
        p_max += q_size;

    if( p_max == r_here )
    {
        p_ready = p_sync;

        p_max = p_ready;

        if( p_max < r_here )
            p_max += q_size;

        if( p_max == r_here )
        {
            //r_penalty();
            return 0;
        }
    }

    pop_max = p_max - r_here;

    if( pop_count == 0 )
        return pop_max;

    if( pop_count > pop_max )
        pop_count = pop_max;

    r_next = r_here + pop_count;

    if( r_next >= q_size )
        r_next -= q_size;

    r_pos = r_next;

    for( int i = 0; i < pop_count; i++ )
    {
        if( r_here + i >= q_size )
            r_here -= q_size;

        assert( q[r_here + i].status == SOQUE_MARKER_PROCESSED );

        q[r_here + i].status = SOQUE_MARKER_EMPTY;
    }

    r_sync = r_next;

    return pop_count;
}

int SOQUE::write_sync( int w_done )
{
    int w_next;

    guard_write gw( this );

    q[w_done].status = SOQUE_MARKER_FILLED;

    if( w_done != w_sync )
        return 0;

    w_next = w_done + 1;

    for( ;; )
    {
        if( w_next == q_size )
            w_next = 0;

        if( w_next == w_pos )
            break;

        if( q[w_next].status != SOQUE_MARKER_FILLED )
            break;

        w_next = w_next + 1;
    }

    w_sync = w_next;

    return 1;
}

void writer( SOQUE_HANDLE q )
{
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    for( ;; )
        soque_push_batch( q, SOQUE_DEFAULT_Q_SIZE );
}

void procer( SOQUE_HANDLE q )
{
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    int pos;
    for( ;; )
        if( ( pos = soque_proc_get( q ) ) != -1 )
        {
            soque_proc_sync( q, pos );
        }
}

void reader( SOQUE_HANDLE q )
{
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    int pos;
    for( ;; )
        if( ( pos = soque_pop_batch( q, SOQUE_DEFAULT_Q_SIZE ) ) != 0 )
            g_read_count += pos;
}

void reader1( SOQUE_HANDLE q )
{
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    int pos;
    for( ;; )
        if( ( pos = soque_read_get( q ) ) != -1 )
        {
            soque_read_sync( q, pos );
            g_read_count++;
        }
}

int push[2] = { 0, 0 };
int pop[2] = { 0, 0 };

std::atomic_bool guard[2];

void crypter( SOQUE_HANDLE * q, int q_count )
{
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

    for( ;; )
    for( int i = 0; i < q_count; i++ )
    {
        {
            int pos;
            int batch = 0;

            for( ;; )
            {
                if( ( pos = soque_proc_get( q[i] ) ) == -1 )
                    break;

                soque_proc_sync( q[i], pos );

                if( ++batch == 256 )
                    break;
            }
        }

        {
            int batch;
            bool f = false;

            if( guard[i].compare_exchange_weak( f, true ) )
            {
                batch = soque_pop_batch( q[i], 0 );
                if( batch )
                {

                    batch = soque_pop_batch( q[i], batch );
                }

                //if( i == 1 )
                    //std::this_thread::sleep_for( std::chrono::milliseconds( 1000 ) );

                if( batch )
                {
                    pop[i] += batch;
                    if( pop[i] >= SOQUE_DEFAULT_Q_SIZE )
                        pop[i] -= SOQUE_DEFAULT_Q_SIZE;

                    g_read_count += batch;
                }

                batch = soque_push_batch( q[i], 0 );
                if( batch )
                    batch = soque_push_batch( q[i], batch );

                if( batch )
                {
                    push[i] += batch;
                    if( push[i] >= SOQUE_DEFAULT_Q_SIZE )
                        push[i] -= SOQUE_DEFAULT_Q_SIZE;
                }

                guard[i] = false;
            }
        }
    }
}

void crypter1( SOQUE_HANDLE q )
{
    std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

    static std::atomic_bool w_guard;
    static std::atomic_bool r_guard;

    bool bf;
    int pos = -1;

    int p_moved = false;
    int r_moved = false;

    for( ;; )
    {
        if( r_moved || pos == -1 )
        {
            bf = false;
            if( w_guard.compare_exchange_weak( bf, true ) )
            {
                soque_push_batch( q, 256 );

                w_guard = false;
                r_moved = false;
            }
        }


        if( ( pos = soque_proc_get( q ) ) != -1 )
        {
            p_moved = soque_proc_sync( q, pos );
        }

        if( p_moved )
        {
            bf = false;
            if( r_guard.compare_exchange_weak( bf, true ) )
            {
                r_moved = soque_pop_batch( q, 256 );
                g_read_count += r_moved;

                r_guard = false;
                p_moved = false;
            }
        }
    }
}

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

SOQUE_HANDLE SOQUE_CALL soque_open( int queue_size,
                         int writers,
                         int procers,
                         int readers,
                         void * cb_arg,
                         soque_push_batch_cb push_cb,
                         soque_push_batch_cb proc_cb,
                         soque_push_batch_cb pop_cb )
{
    void * mem = malloc( sizeof( SOQUE ) + sizeof( SOQUE::MARKER ) * queue_size + CACHELINE_SIZE );

    if( !mem )
        return NULL;

    SOQUE_HANDLE sh = CACHELINE_SHIFT( mem, SOQUE_HANDLE );
    sh->init( queue_size, writers, procers, readers, cb_arg, push_cb, proc_cb, pop_cb );
    sh->mem = mem;

    return sh;
}

int SOQUE_CALL soque_read_get( SOQUE_HANDLE sh )
{
    return sh->read_get();
}

int SOQUE_CALL soque_read_sync( SOQUE_HANDLE sh, int read_done )
{
    return sh->read_sync( read_done );
}

int SOQUE_CALL soque_proc_get( SOQUE_HANDLE sh )
{
    return sh->proc_get();
}

int SOQUE_CALL soque_proc_sync( SOQUE_HANDLE sh, int proc_done )
{
    return sh->proc_sync( proc_done );
}

int SOQUE_CALL soque_write_get( SOQUE_HANDLE sh )
{
    return sh->write_get();
}

int SOQUE_CALL soque_write_sync( SOQUE_HANDLE sh, int write_done )
{
    return sh->write_sync( write_done );
}

int SOQUE_CALL soque_push_batch( SOQUE_HANDLE sh, int push_count )
{
    return sh->push_batch( push_count );
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
            vt.push_back( std::thread( &ttt, this ) );

        for( int i = 0; i < threads_count; i++ )
            sit_on_cpu( vt[i] );

        return 1;
    }

    static void ttt( SOQUE_THREADS * sts )
    {
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

        int i = 0;

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

                    soque_proc_sync( sh, pos );

                    if( ++batch == 256 )
                        break;
                }
            }

            // pop + push
            {
                int batch;
                bool f = false;

                if( guard[i].compare_exchange_weak( f, true ) )
                {
                    batch = soque_pop_batch( sh, 0 );
                    
                    if( batch )
                    {
                        batch = sh->pop_cb( sh->cb_arg, batch );

                        if( batch )
                        {
                            batch = soque_pop_batch( sh, batch );

                            if( batch )
                            {
                                pop[i] += batch;
                                if( pop[i] >= SOQUE_DEFAULT_Q_SIZE )
                                    pop[i] -= SOQUE_DEFAULT_Q_SIZE;

                                g_read_count += batch;
                            }
                        }
                    }

                    batch = soque_push_batch( sh, 0 );

                    if( batch )
                    {
                        batch = sh->push_cb( sh->cb_arg, batch );

                        if( batch )
                        {
                            batch = soque_push_batch( sh, batch );

                            if( batch )
                            {
                                push[i] += batch;
                                if( push[i] >= SOQUE_DEFAULT_Q_SIZE )
                                    push[i] -= SOQUE_DEFAULT_Q_SIZE;
                            }
                        }
                    }

                    guard[i] = false;
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

int SOQUE_CALL soque_cb( void * arg, int count )
{
    (void)arg;

    return count;
}

soque_push_batch_cb push_cb = &soque_cb;
soque_push_batch_cb proc_cb = &soque_cb;
soque_push_batch_cb pop_cb = &soque_cb;

int main( int argc, char ** argv )
{
    int q_count = SOQUE_DEFAULT_Q_SIZE;
    int w_count = 1;
    int p_count = 1;
    int r_count = 1;

    if( argc == 6 )
    {
        q_count = atoi( argv[1] );
        w_count = atoi( argv[2] );
        p_count = atoi( argv[3] );
        r_count = atoi( argv[4] );
        SOQUE_PENALTY = atoi( argv[5] );
    }

    printf( "q_count = %d\n", q_count );
    printf( "w_count = %d\n", w_count );
    printf( "p_count = %d\n", p_count );
    printf( "r_count = %d\n", r_count );
    printf( "SOQ_PENALTY = %d\n\n", SOQUE_PENALTY );

    SOQUE_HANDLE q[4];
    q[2] = soque_open( q_count, w_count, p_count, r_count, NULL, NULL, NULL, NULL );
    q[3] = soque_open( q_count, w_count, p_count, r_count, NULL, NULL, NULL, NULL );

    q[0] = soque_open( q_count, w_count, p_count, r_count, q[2], push_cb, proc_cb, (soque_pop_batch_cb)&soque_push_batch );
    q[1] = soque_open( q_count, w_count, p_count, r_count, q[2], (soque_push_batch_cb)&soque_pop_batch, proc_cb, pop_cb );

    //q[0] = soque_init( q_count, w_count, p_count, r_count, q[2], push_cb, proc_cb, pop_cb );
    //q[1] = soque_init( q_count, w_count, p_count, r_count, q[2], push_cb, proc_cb, pop_cb );

    printf( "sizeof( struct StrictOrderQueue ) = %d\n", ( int )sizeof( SOQUE ) );
    printf( "sizeof( struct SOQUE::QWORKER ) = %d\n", ( int )sizeof( SOQUE::MARKER ) );
    printf( "sizeof( struct SOQUE_THREAD ) = %d\n\n", ( int )sizeof( SOQUE_THREAD ) );


    SOQUE_THREADS_HANDLE sth;
    sth = soque_threads_open( p_count, q, 2 );

    std::this_thread::sleep_for( std::chrono::milliseconds( 1000 ) ); // warming

    double speed_moment = 0;
    double speed_approx = 0;
    int n = 0;

    std::thread temp( [&](){
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
