#define SOQUE_MAJOR 1
#define SOQUE_MINOR 0 

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#define SOQUE_API __declspec( dllexport )
#define SOQUE_CALL __fastcall
#ifdef _M_IX86
#define SOQUE_LIBRARY "soque.dll"
#else // _M_IX86
#define SOQUE_LIBRARY "soque64.dll"
#endif // _M_IX86
#else // _WIN32
#define SOQUE_API __attribute__( ( visibility( "default" ) ) )
#define SOQUE_CALL 
#define SOQUE_LIBRARY "libsoque.so"
#endif // _WIN32
#define SOQUE_GET_FRAMEWORK "soque_framework"

    typedef int ( SOQUE_CALL * soque_push_cb )( void * cb_arg, int push_count, int waitable );
    typedef void ( SOQUE_CALL * soque_proc_cb )( void * cb_arg, int proc_count, int proc_index );
    typedef int ( SOQUE_CALL * soque_pop_cb )( void * cb_arg, int pop_count, int waitable );

    typedef struct SOQUE * SOQUE_HANDLE;

    typedef SOQUE_HANDLE ( SOQUE_CALL * soque_open_t )( int size, void * cb_arg, soque_push_cb, soque_proc_cb, soque_pop_cb );
    typedef int ( SOQUE_CALL * soque_push_t )( SOQUE_HANDLE, int push_count );
    typedef int ( SOQUE_CALL * soque_proc_open_t )( SOQUE_HANDLE, int proc_count, int * proc_index );
    typedef int ( SOQUE_CALL * soque_proc_done_t )( SOQUE_HANDLE, int proc_count, int proc_index );
    typedef int ( SOQUE_CALL * soque_pop_t )( SOQUE_HANDLE, int pop_count );
    typedef void ( SOQUE_CALL * soque_done_t )( SOQUE_HANDLE );

    typedef struct SOQUE_THREADS * SOQUE_THREADS_HANDLE;

    typedef SOQUE_THREADS_HANDLE ( SOQUE_CALL * soque_threads_open_t )( int threads, SOQUE_HANDLE * shs, int shs_count );
    typedef void ( SOQUE_CALL * soque_threads_done_t )( SOQUE_THREADS_HANDLE );

    typedef struct {
        int soque_major;
        int soque_minor;
        soque_open_t soque_open;
        soque_push_t soque_push;
        soque_proc_open_t soque_proc_open;
        soque_proc_done_t soque_proc_done;
        soque_pop_t soque_pop;
        soque_done_t soque_done;
        soque_threads_open_t soque_threads_open;
        soque_threads_done_t soque_threads_done;
    } SOQUE_FRAMEWORK;

    typedef SOQUE_FRAMEWORK * ( SOQUE_CALL * soque_framework_t )();
    SOQUE_API SOQUE_FRAMEWORK * SOQUE_CALL soque_framework();

#ifdef __cplusplus
}
#endif
