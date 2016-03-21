#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#define SOQUE_API __declspec( dllexport )
#define SOQUE_CALL __fastcall
#else
#define SOQUE_API __attribute__( ( visibility( "default" ) ) )
#define SOQUE_CALL 
#endif

    typedef int ( SOQUE_CALL * soque_push_cb )( void * cb_arg, int push_count, int waitable );
    typedef void ( SOQUE_CALL * soque_proc_cb )( void * cb_arg, int proc_count, int proc_index );
    typedef int ( SOQUE_CALL * soque_pop_cb )( void * cb_arg, int pop_count, int waitable );

    typedef struct SOQUE * SOQUE_HANDLE;

    SOQUE_API SOQUE_HANDLE SOQUE_CALL soque_open( int size, int threads, void * cb_arg, soque_push_cb, soque_proc_cb, soque_pop_cb );
    SOQUE_API int SOQUE_CALL soque_push( SOQUE_HANDLE sh, int push_count );
    SOQUE_API int SOQUE_CALL soque_proc_open( SOQUE_HANDLE sh, int proc_count, int * proc_index );
    SOQUE_API int SOQUE_CALL soque_proc_done( SOQUE_HANDLE sh, int proc_count, int proc_index );
    SOQUE_API int SOQUE_CALL soque_pop( SOQUE_HANDLE sh, int pop_count );
    SOQUE_API void SOQUE_CALL soque_done( SOQUE_HANDLE sh );

    typedef struct SOQUE_THREADS * SOQUE_THREADS_HANDLE;

    SOQUE_API SOQUE_THREADS_HANDLE SOQUE_CALL soque_threads_open( int threads, SOQUE_HANDLE * shs, int shs_count );
    SOQUE_API void SOQUE_CALL soque_threads_done( SOQUE_THREADS_HANDLE sth );

#ifdef __cplusplus
}
#endif
