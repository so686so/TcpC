/**
 * 파일명: src/TcpServer.c
 * 개요: TcpServer.h 에 선언된 서버 컨텍스트의 핵심 구현부
 *
 * [아키텍처]
 * 1. Main Thread (Epoll): 연결 수락(Accept) -> Handshake -> 리스트 추가 -> 데이터 수신(Recv) -> RecvQueue Push
 * 2. Worker Threads: RecvQueue Pop -> 패킷 파싱 -> 비즈니스 로직(Callback) -> (필요시) SendQueue Push
 * 3. Sender Thread: SendQueue Pop -> 패킷 직렬화 -> 암호화 -> 실제 전송(Send/Broadcast)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>

#include "TcpServer.h"
#include "PacketUtils.h"

// --------------------------------------------------------------------------
// 1. 내부 구조체 정의 (ClientNode 은닉)
// --------------------------------------------------------------------------

typedef struct ClientNode
{
    int fd;
    struct ClientNode* next;
} ClientNode;


// --------------------------------------------------------------------------
// 2. 클라이언트 리스트 관리 함수 (Context 내부 멤버 사용)
// --------------------------------------------------------------------------

/**
 * ## 연결된 클라이언트 FD를 리스트에 추가한다. (Thread-Safe)
 */
static void AddClient( TcpServerContext* ctx, int fd )
{
    ClientNode* node = (ClientNode*)malloc( sizeof( ClientNode ) );
    if( !node ) return;

    node->fd = fd;
    node->next = NULL;

    pthread_mutex_lock( &ctx->client_list_mutex );
    {
        node->next = ctx->client_list_head;
        ctx->client_list_head = node;
        ctx->current_client_count++;
    }
    pthread_mutex_unlock( &ctx->client_list_mutex );
    printf("[AddClient] FD : %d\n", fd);
}

/**
 * ## 연결 해제된 클라이언트 FD를 리스트에서 제거한다. (Thread-Safe)
 */
static void RemoveClient( TcpServerContext* ctx, int fd )
{
    pthread_mutex_lock( &ctx->client_list_mutex );
    {
        ClientNode* curr = ctx->client_list_head;
        ClientNode* prev = NULL;

        while( curr != NULL )
        {
            if( curr->fd == fd )
            {
                if( prev == NULL ) ctx->client_list_head = curr->next;
                else prev->next = curr->next;

                free( curr );
                ctx->current_client_count--;
                break;
            }
            prev = curr;
            curr = curr->next;
        }
    }
    pthread_mutex_unlock( &ctx->client_list_mutex );
    printf("[RemoveClient] FD : %d\n", fd);
}


// --------------------------------------------------------------------------
// 3. 헬퍼 함수
// --------------------------------------------------------------------------

static void SetNonBlocking( int fd )
{
    int flags = fcntl( fd, F_GETFL, 0 );
    fcntl( fd, F_SETFL, flags | O_NONBLOCK );
}

// --------------------------------------------------------------------------
// 4. 큐 데이터 해제 콜백 (SafeQueue_Destroy용)
// --------------------------------------------------------------------------

static void FreeRecvTask( void* data )
{
    ServerRecvTask* task = (ServerRecvTask*)data;
    if( task )
    {
        if( task->data ) free( task->data );
        free( task );
    }
}

static void FreeSendTask( void* data )
{
    ServerSendTask* task = (ServerSendTask*)data;
    if( task )
    {
        if( task->body_data ) free( task->body_data );
        free( task );
    }
}


// --------------------------------------------------------------------------
// 5. 워커 스레드 (Worker Thread)
// 설명: 수신된 Raw 데이터를 파싱하고 사용자 콜백을 호출한다.
// --------------------------------------------------------------------------

static void* WorkerThreadFunc( void* arg )
{
    TcpServerContext* ctx = (TcpServerContext*)arg;

    while( ctx->is_running )
    {
        // 1. 큐에서 작업 가져오기 (Blocking)
        ServerRecvTask* task = (ServerRecvTask*)SafeQueue_Dequeue( ctx->recv_queue );

        // 2. 종료 신호(Poison Pill) 확인
        // task가 NULL이거나, fd가 -1인 경우 종료로 간주
        if( !task || task->client_fd == -1 )
        {
            if( task ) FreeRecvTask( task );
            break;
        }

        // 3. 패킷 파싱
        char target_buf[TARGET_NAME_LEN + 1];
        char* body_ptr = NULL;
        int body_len = 0;

        // In-place decryption을 위해 task->data(힙 메모리)를 바로 넘김
        PacketResult result = Packet_Parse( task->data, task->len,
                                            ctx->decrypt_fn,
                                            target_buf,
                                            &body_ptr, &body_len );

        if( result == PKT_SUCCESS )
        {
            // 4. 사용자 콜백 호출 (비즈니스 로직)
            if( ctx->on_message )
            {
                ctx->on_message( ctx, task->client_fd, ctx->user_arg,
                                 target_buf, body_ptr, body_len );
            }
        }
        else
        {
            // 파싱 실패 시 로그 (운영 환경에선 파일 로그 권장)
            // printf( "[Worker] Parse failed (FD: %d, Err: %d)\n", task->client_fd, result );
        }

        // 5. 작업 메모리 해제
        FreeRecvTask( task );
    }

    return NULL;
}


// --------------------------------------------------------------------------
// 6. 송신 스레드 (Sender Thread)
// 설명: 전송 요청을 직렬화하여 소켓에 쓴다. (Broadcast 지원)
// --------------------------------------------------------------------------

static void* SenderThreadFunc( void* arg )
{
    TcpServerContext* ctx = (TcpServerContext*)arg;

    // 직렬화용 임시 버퍼 (스레드 로컬)
    char* send_buf = (char*)malloc( DEFAULT_BUF_SIZE );
    if( !send_buf ) return NULL;

    while( ctx->is_running )
    {
        // 1. 큐에서 전송 요청 가져오기
        ServerSendTask* task = (ServerSendTask*)SafeQueue_Dequeue( ctx->send_queue );

        // 2. 종료 신호 확인 (-2: Sender 종료 코드)
        if( !task || task->client_fd == -2 )
        {
            if( task ) FreeSendTask( task );
            break;
        }

        // 3. 패킷 직렬화 (암호화 포함)
        int packet_len = Packet_Serialize( send_buf, DEFAULT_BUF_SIZE,
                                           task->target,
                                           task->body_data, task->body_len,
                                           ctx->encrypt_fn );

        if( packet_len > 0 )
        {
            if( task->is_broadcast )
            {
                // 4-A. 브로드캐스트 전송
                // 리스트 순회 시 Mutex 잠금 필수
                pthread_mutex_lock( &ctx->client_list_mutex );
                {
                    ClientNode* curr = ctx->client_list_head;
                    while( curr != NULL )
                    {
                        // MSG_NOSIGNAL: 상대방 연결 끊김 시 SIGPIPE 시그널 발생 방지
                        send( curr->fd, send_buf, packet_len, MSG_NOSIGNAL );
                        curr = curr->next;
                    }
                }
                pthread_mutex_unlock( &ctx->client_list_mutex );
            }
            else
            {
                // 4-B. 유니캐스트 전송
                send( task->client_fd, send_buf, packet_len, MSG_NOSIGNAL );
            }
        }

        // 작업 완료 후 해제
        FreeSendTask( task );
    }

    free( send_buf );
    return NULL;
}


// --------------------------------------------------------------------------
// 7. 멤버 함수 구현
// --------------------------------------------------------------------------

static bool impl_Server_Init( TcpServerContext* ctx, int port )
{
    if( !ctx ) return false;

    // Epoll 생성
    ctx->epoll_fd = epoll_create1( 0 );
    if( ctx->epoll_fd < 0 ) return false;

    // Listen 소켓 생성
    ctx->listen_fd = socket( AF_INET, SOCK_STREAM, 0 );
    if( ctx->listen_fd < 0 ) return false;

    // 주소 재사용 옵션 (서버 재시작 시 Bind Error 방지)
    int opt = 1;
    setsockopt( ctx->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof( opt ) );

    // 바인딩
    struct sockaddr_in addr;
    memset( &addr, 0, sizeof( addr ) );
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons( port );

    if( bind( ctx->listen_fd, (struct sockaddr*)&addr, sizeof( addr ) ) < 0 )
    {
        perror( "[TcpServer] Bind failed" );
        return false;
    }

    if( listen( ctx->listen_fd, 100 ) < 0 )
    {
        perror( "[TcpServer] Listen failed" );
        return false;
    }

    // Epoll 처리를 위해 Non-blocking 설정
    SetNonBlocking( ctx->listen_fd );

    // Epoll에 Listen 소켓 등록
    struct epoll_event ev;
    ev.events = EPOLLIN; // 읽기 이벤트 감지
    ev.data.fd = ctx->listen_fd;
    if( epoll_ctl( ctx->epoll_fd, EPOLL_CTL_ADD, ctx->listen_fd, &ev ) < 0 )
    {
        perror( "[TcpServer] Epoll control failed" );
        return false;
    }

    // Epoll 이벤트 버퍼 할당
    ctx->events = (struct epoll_event*)malloc( sizeof( struct epoll_event ) * MAX_EPOLL_EVENTS );

    printf( "[TcpServer] Initialized on port %d\n", port );
    return true;
}

static void impl_Server_Run( TcpServerContext* ctx, volatile bool* exit_flag )
{
    if( !ctx ) return;

    ctx->is_running = true;

    // 1. 워커 스레드 생성
    for( int i = 0; i < ctx->worker_count; ++i )
    {
        pthread_create( &ctx->worker_threads[i], NULL, WorkerThreadFunc, ctx );
    }

    // 2. 송신 스레드 생성
    pthread_create( &ctx->sender_thread, NULL, SenderThreadFunc, ctx );

    printf( "[TcpServer] Server loop started (Epoll).\n" );

    // 3. Epoll 루프 (Main IO Thread)
    while( ctx->is_running )
    {
        // 외부 종료 플래그 체크
        if( exit_flag && *exit_flag )
        {
            printf( "[TcpServer] Stop signal detected. Exiting loop...\n" );
            break;
        }

        // 100ms 타임아웃으로 대기 (종료 시그널 체크를 위해)
        int n_fds = epoll_wait( ctx->epoll_fd, ctx->events, MAX_EPOLL_EVENTS, 100 );

        if( n_fds < 0 )
        {
            if( errno == EINTR ) continue; // 시그널 인터럽트는 무시
            perror( "[TcpServer] Epoll wait error" );
            break;
        }

        for( int i = 0; i < n_fds; ++i )
        {
            int curr_fd = ctx->events[i].data.fd;

            // [Case A] 새로운 클라이언트 접속
            if( curr_fd == ctx->listen_fd )
            {
                struct sockaddr_in cli_addr;
                socklen_t cli_len = sizeof( cli_addr );
                int client_fd = accept( ctx->listen_fd, (struct sockaddr*)&cli_addr, &cli_len );

                if( client_fd >= 0 )
                {
                    SetNonBlocking( client_fd );

                    // Epoll 등록 (Edge Triggered)
                    struct epoll_event ev;
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = client_fd;
                    epoll_ctl( ctx->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev );

                    // 내부 리스트에 추가
                    AddClient( ctx, client_fd );

                    // [핸드셰이크] 암호화 정보 전송 (요구사항 5번)
                    // 현재는 단순 문자열 전송. 실제로는 패킷 포맷을 맞춰 보내기도 함.
                    const char* handshake_msg = "ENC:XOR";
                    send( client_fd, handshake_msg, strlen( handshake_msg ), 0 );

                    printf( "[TcpServer] Client %d connected.\n", client_fd );
                }
            }
            // [Case B] 데이터 수신 (From Client)
            else
            {
                // Edge Triggered 모드이므로 루프를 돌며 다 읽거나,
                // 여기서는 단순화하여 버퍼 크기만큼 읽음. (실제 상용에선 RingBuffer 필수)
                char* temp_buf = (char*)malloc( DEFAULT_BUF_SIZE );
                if( !temp_buf ) continue;

                int len = recv( curr_fd, temp_buf, DEFAULT_BUF_SIZE, 0 );

                if( len > 0 )
                {
                    // 수신된 데이터를 Task로 포장하여 RecvQueue로 전달
                    ServerRecvTask* task = (ServerRecvTask*)malloc( sizeof( ServerRecvTask ) );
                    if( task )
                    {
                        task->client_fd = curr_fd;
                        task->data = temp_buf; // 메모리 소유권 이전
                        task->len = len;

                        // 큐가 가득 찼으면 Drop (Backpressure)
                        if( !SafeQueue_Enqueue( ctx->recv_queue, task ) )
                        {
                            printf( "[TcpServer] RecvQueue Full! Dropping packet from %d\n", curr_fd );
                            FreeRecvTask( task ); // task와 data 모두 해제됨
                        }
                    }
                    else
                    {
                        free( temp_buf );
                    }
                }
                else
                {
                    // 연결 종료 (0) 또는 에러 (<0)
                    close( curr_fd ); // Epoll에서 자동 제거됨
                    RemoveClient( ctx, curr_fd );
                    free( temp_buf );

                    // printf( "[TcpServer] Client %d disconnected.\n", curr_fd );
                }
            }
        }
    }
}

static bool impl_Server_Send( TcpServerContext* ctx, int client_fd, const char* target, void* body, int len )
{
    if( !ctx || !ctx->is_running ) return false;

    // SendTask 생성
    ServerSendTask* task = (ServerSendTask*)malloc( sizeof( ServerSendTask ) );
    if( !task ) return false;

    task->client_fd = client_fd;
    task->is_broadcast = false;
    memset( task->target, 0, TARGET_NAME_LEN );
    if( target ) strncpy( task->target, target, TARGET_NAME_LEN );

    // 바디 데이터 Deep Copy (비동기 전송을 위해 필수)
    if( body && len > 0 )
    {
        task->body_data = (char*)malloc( len );
        if( !task->body_data )
        {
            free( task );
            return false;
        }
        memcpy( task->body_data, body, len );
        task->body_len = len;
    }
    else
    {
        task->body_data = NULL;
        task->body_len = 0;
    }

    if( !SafeQueue_Enqueue( ctx->send_queue, task ) )
    {
        FreeSendTask( task );
        return false;
    }

    return true;
}

static bool impl_Server_Broadcast( TcpServerContext* ctx, const char* target, void* body, int len )
{
    if( !ctx || !ctx->is_running ) return false;

    ServerSendTask* task = (ServerSendTask*)malloc( sizeof( ServerSendTask ) );
    if( !task ) return false;

    task->client_fd = -1; // Broadcast에서는 무시됨
    task->is_broadcast = true;
    memset( task->target, 0, TARGET_NAME_LEN );
    if( target ) strncpy( task->target, target, TARGET_NAME_LEN );

    if( body && len > 0 )
    {
        task->body_data = (char*)malloc( len );
        if( !task->body_data )
        {
            free( task );
            return false;
        }
        memcpy( task->body_data, body, len );
        task->body_len = len;
    }
    else
    {
        task->body_data = NULL;
        task->body_len = 0;
    }

    if( !SafeQueue_Enqueue( ctx->send_queue, task ) )
    {
        FreeSendTask( task );
        return false;
    }

    return true;
}

static void impl_Server_SetStrategy( TcpServerContext* ctx, EncryptFunc enc, DecryptFunc dec )
{
    if( ctx )
    {
        ctx->encrypt_fn = enc;
        ctx->decrypt_fn = dec;
    }
}

static void impl_Server_Destroy( TcpServerContext* ctx )
{
    if( !ctx ) return;

    ctx->is_running = false; // 루프 종료 플래그

    // 1. 스레드 종료 신호 전송 (Poison Pill)

    // 모든 워커를 깨우기 위해 워커 수만큼 종료 태스크 삽입
    for( int i = 0; i < ctx->worker_count; ++i )
    {
        ServerRecvTask* poison = (ServerRecvTask*)malloc( sizeof( ServerRecvTask ) );
        if( poison )
        {
            poison->client_fd = -1; // 종료 코드
            poison->data = NULL;
            SafeQueue_Enqueue( ctx->recv_queue, poison );
        }
    }

    // 송신 스레드용 종료 태스크
    ServerSendTask* poison_send = (ServerSendTask*)malloc( sizeof( ServerSendTask ) );
    if( poison_send )
    {
        poison_send->client_fd = -2; // 종료 코드
        poison_send->body_data = NULL;
        SafeQueue_Enqueue( ctx->send_queue, poison_send );
    }

    // 2. 스레드 종료 대기 (Join)
    for( int i = 0; i < ctx->worker_count; ++i )
    {
        pthread_join( ctx->worker_threads[i], NULL );
    }
    pthread_join( ctx->sender_thread, NULL );

    // 3. 자원 해제
    if( ctx->listen_fd >= 0 ) close( ctx->listen_fd );
    if( ctx->epoll_fd >= 0 ) close( ctx->epoll_fd );

    // 큐 파괴 (SafeQueue_Destroy가 내부 데이터까지 FreeRecvTask/FreeSendTask 호출로 정리함)
    SafeQueue_Destroy( ctx->recv_queue, FreeRecvTask );
    SafeQueue_Destroy( ctx->send_queue, FreeSendTask );

    // 클라이언트 리스트 정리
    pthread_mutex_lock( &ctx->client_list_mutex );
    {
        ClientNode* curr = ctx->client_list_head;
        while( curr != NULL )
        {
            ClientNode* next = curr->next;
            close( curr->fd ); // 아직 안 닫힌 소켓 정리
            free( curr );
            curr = next;
        }
        ctx->client_list_head = NULL;
    }
    pthread_mutex_unlock( &ctx->client_list_mutex );
    pthread_mutex_destroy( &ctx->client_list_mutex );

    if( ctx->events ) free( ctx->events );
    if( ctx->worker_threads ) free( ctx->worker_threads );

    free( ctx );

    printf( "[TcpServer] Destroyed successfully.\n" );
}

static int impl_GetClientCount( TcpServerContext* ctx )
{
    if( !ctx )
        return 0;
    // Mutex를 걸고 읽을 수도 있지만, volatile int는 읽기 atomic을 기대하고 단순 반환 (통계용)
    return ctx->current_client_count;
}

// --------------------------------------------------------------------------
// 8. 생성자 구현
// --------------------------------------------------------------------------

TcpServerContext* CreateTcpServerContext( OnServerMessageCallback callback, void* user_arg, int worker_count )
{
    TcpServerContext* ctx = (TcpServerContext*)malloc( sizeof( TcpServerContext ) );
    if( !ctx ) return NULL;

    memset( ctx, 0, sizeof( TcpServerContext ) );

    // 기본값 설정
    ctx->listen_fd = -1;
    ctx->epoll_fd = -1;
    ctx->on_message = callback;
    ctx->user_arg = user_arg;
    ctx->worker_count = ( worker_count > 0 ) ? worker_count : DEFAULT_WORKER_NUM;
    ctx->current_client_count = 0;

    // 클라이언트 리스트 초기화
    ctx->client_list_head = NULL;
    if( pthread_mutex_init( &ctx->client_list_mutex, NULL ) != 0 )
    {
        free( ctx );
        return NULL;
    }

    // 전략 설정 (기본값)
    ctx->encrypt_fn = Packet_DefaultXor;
    ctx->decrypt_fn = Packet_DefaultXor;

    // 큐 생성
    ctx->recv_queue = SafeQueue_Create( QUEUE_CAPACITY );
    ctx->send_queue = SafeQueue_Create( QUEUE_CAPACITY );

    if( !ctx->recv_queue || !ctx->send_queue )
    {
        // 큐 생성 실패 시 정리 (상세 구현 생략)
        free( ctx );
        return NULL;
    }

    // 스레드 배열 할당
    ctx->worker_threads = (pthread_t*)malloc( sizeof( pthread_t ) * ctx->worker_count );

    // 함수 바인딩
    ctx->Init = impl_Server_Init;
    ctx->Run = impl_Server_Run;
    ctx->Send = impl_Server_Send;
    ctx->Broadcast = impl_Server_Broadcast;
    ctx->SetStrategy = impl_Server_SetStrategy;
    ctx->Destroy = impl_Server_Destroy;
    ctx->GetClientCount = impl_GetClientCount;

    return ctx;
}