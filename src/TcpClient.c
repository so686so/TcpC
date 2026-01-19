/**
 * 파일명: src/TcpClient.c
 * 개요: TcpClient.h 에 선언된 클라이언트 기능 구현부
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <errno.h>

#include "TcpClient.h"
#include "PacketUtils.h"

// --------------------------------------------------------------------------
// 내부 헬퍼 함수 선언
// --------------------------------------------------------------------------

/**
 * ## 지정한 바이트 수만큼 데이터를 확실하게 읽어온다. (Blocking)
 * #### MSG_WAITALL을 사용하거나 루프를 돌며 완전한 읽기를 보장한다.
 */
static int RecvExact( int fd, char* buf, int len )
{
    int total_read = 0;
    while( total_read < len )
    {
        int received = recv( fd, buf + total_read, len - total_read, 0 );
        if( received <= 0 )
        {
            return received; // 0: Close, -1: Error
        }
        total_read += received;
    }
    return total_read;
}

// --------------------------------------------------------------------------
// 네트워크 관리 스레드 (재연결 + 수신)
// --------------------------------------------------------------------------

static void* NetworkManagerThread( void* arg )
{
    TcpClientContext* ctx = (TcpClientContext*)arg;
    char* recv_buf = (char*)malloc( DEFAULT_BUF_SIZE );

    printf( "[TcpClient] Network Manager Started. Target: %s:%d\n", ctx->server_ip, ctx->server_port );

    while( ctx->is_running )
    {
        // ---------------------------------------------------------
        // Phase 1: 연결 시도 (Disconnected 상태일 때)
        // ---------------------------------------------------------
        int curr_fd = -1;
        pthread_mutex_lock( &ctx->conn_mutex );
        curr_fd = ctx->sockfd;
        pthread_mutex_unlock( &ctx->conn_mutex );

        if( curr_fd == -1 )
        {
            // 소켓 생성
            int sock = socket( AF_INET, SOCK_STREAM, 0 );
            if( sock < 0 ) { sleep( 1 ); continue; }

            struct sockaddr_in serv_addr;
            memset( &serv_addr, 0, sizeof( serv_addr ) );
            serv_addr.sin_family = AF_INET;
            serv_addr.sin_port = htons( ctx->server_port );
            inet_pton( AF_INET, ctx->server_ip, &serv_addr.sin_addr );

            // 연결 시도
            if( connect( sock, (struct sockaddr*)&serv_addr, sizeof( serv_addr ) ) < 0 )
            {
                // 실패 시 잠시 대기 후 재시도
                // printf( "[TcpClient] Connect failed.. retrying in 1s.\n" );
                close( sock );
                sleep( 1 );
                continue;
            }

            // 연결 성공 -> 핸드셰이크
            char handshake_buf[8];
            if( RecvExact( sock, handshake_buf, 7 ) != 7 )
            {
                close( sock );
                sleep( 1 );
                continue;
            }

            if( strncmp( handshake_buf, "ENC:XOR", 7 ) == 0 )
            {
                printf( "[TcpClient] Connected to Server! (Handshake OK)\n" );

                // 소켓 등록 (Send 함수가 쓸 수 있게)
                pthread_mutex_lock( &ctx->conn_mutex );
                ctx->sockfd = sock;
                pthread_mutex_unlock( &ctx->conn_mutex );
            }
            else
            {
                close( sock );
                sleep( 1 );
                continue;
            }
        }

        // ---------------------------------------------------------
        // Phase 2: 데이터 수신 루프 (Connected 상태)
        // ---------------------------------------------------------

        // 현재 소켓 가져오기
        pthread_mutex_lock( &ctx->conn_mutex );
        curr_fd = ctx->sockfd;
        pthread_mutex_unlock( &ctx->conn_mutex );

        if( curr_fd != -1 )
        {
            // 1. 헤더 읽기
            int header_size = sizeof( PacketHeader );
            int ret = RecvExact( curr_fd, recv_buf, header_size );

            if( ret <= 0 )
            {
                // 연결 끊김 감지
                printf( "[TcpClient] Disconnected from server. Reconnecting...\n" );

                pthread_mutex_lock( &ctx->conn_mutex );
                close( ctx->sockfd );
                ctx->sockfd = -1; // 다시 연결 시도 모드로 전환
                pthread_mutex_unlock( &ctx->conn_mutex );
                continue;
            }

            // 2. 바디 읽기
            PacketHeader* peek_header = (PacketHeader*)recv_buf;
            uint32_t total_len = ntohl( peek_header->total_len );

            if( total_len > DEFAULT_BUF_SIZE || total_len < header_size )
            {
                printf( "[TcpClient] Invalid Packet. Reconnecting...\n" );
                pthread_mutex_lock( &ctx->conn_mutex );
                close( ctx->sockfd );
                ctx->sockfd = -1;
                pthread_mutex_unlock( &ctx->conn_mutex );
                continue;
            }

            int remain_len = total_len - header_size;
            if( remain_len > 0 )
            {
                if( RecvExact( curr_fd, recv_buf + header_size, remain_len ) <= 0 )
                {
                     pthread_mutex_lock( &ctx->conn_mutex );
                     close( ctx->sockfd );
                     ctx->sockfd = -1;
                     pthread_mutex_unlock( &ctx->conn_mutex );
                     continue;
                }
            }

            // 3. 파싱 및 콜백
            char target_buf[TARGET_NAME_LEN + 1];
            char* body_ptr = NULL;
            int body_len = 0;

            if( Packet_Parse( recv_buf, total_len, ctx->decrypt_fn, target_buf, &body_ptr, &body_len ) == PKT_SUCCESS )
            {
                if( ctx->on_message )
                    ctx->on_message( ctx, ctx->user_arg, target_buf, body_ptr, body_len );
            }
        }
    } // while(is_running)

    if( recv_buf ) free( recv_buf );

    // 루프 종료 시 정리
    pthread_mutex_lock( &ctx->conn_mutex );
    if( ctx->sockfd != -1 ) {
        close( ctx->sockfd );
        ctx->sockfd = -1;
    }
    pthread_mutex_unlock( &ctx->conn_mutex );

    return NULL;
}

// --------------------------------------------------------------------------
// 멤버 함수 구현
// --------------------------------------------------------------------------

static bool impl_Connect( TcpClientContext* ctx, const char* ip, int port )
{
    if( !ctx ) return false;

    strncpy( ctx->server_ip, ip, sizeof( ctx->server_ip ) - 1 );
    ctx->server_port = port;
    ctx->is_running = true;

    // 스레드 시작
    if( pthread_create( &ctx->network_thread, NULL, NetworkManagerThread, ctx ) != 0 )
    {
        return false;
    }
    return true;
}

static void impl_Disconnect( TcpClientContext* ctx )
{
    if( !ctx ) return;
    ctx->is_running = false;

    // 소켓 강제 종료로 recv 블로킹 해제 유도
    pthread_mutex_lock( &ctx->conn_mutex );
    if( ctx->sockfd != -1 ) {
        shutdown( ctx->sockfd, SHUT_RDWR );
        close( ctx->sockfd );
        ctx->sockfd = -1;
    }
    pthread_mutex_unlock( &ctx->conn_mutex );

    pthread_join( ctx->network_thread, NULL );
}

static int impl_Send( TcpClientContext* ctx, const char* target, void* body, int len )
{
    if( !ctx || !ctx->is_running ) return -1;

    int fd = -1;
    pthread_mutex_lock( &ctx->conn_mutex );
    fd = ctx->sockfd;
    pthread_mutex_unlock( &ctx->conn_mutex );

    if( fd == -1 ) return -1; // 연결 안됨

    char* send_buf = (char*)malloc( DEFAULT_BUF_SIZE );
    int pkt_len = Packet_Serialize( send_buf, DEFAULT_BUF_SIZE, target, body, len, ctx->encrypt_fn );

    int sent = -1;
    if( pkt_len > 0 )
    {
        sent = send( fd, send_buf, pkt_len, MSG_NOSIGNAL );
    }
    free( send_buf );
    return sent;
}

static void impl_SetStrategy( TcpClientContext* ctx, EncryptFunc enc, DecryptFunc dec )
{
    if( ctx )
    {
        // 스레드 안전성을 위해 필요하다면 Mutex를 써야 하지만,
        // 보통 함수 포인터 대입은 원자적(Atomic)으로 간주하여 단순 대입 처리함.
        ctx->encrypt_fn = enc;
        ctx->decrypt_fn = dec;
    }
}

static void impl_Destroy( TcpClientContext* ctx )
{
    if( !ctx ) return;

    if( ctx->is_running )
        ctx->Disconnect( ctx );

    pthread_mutex_destroy( &ctx->conn_mutex );
    free( ctx );

    printf( "[TcpClient] Context destroyed.\n" );
}


// --------------------------------------------------------------------------
// 생성자 구현
// --------------------------------------------------------------------------

TcpClientContext* CreateTcpClientContext( OnMessageCallback callback, void* user_arg )
{
    TcpClientContext* ctx = (TcpClientContext*)malloc( sizeof( TcpClientContext ) );
    memset( ctx, 0, sizeof( TcpClientContext ) );

    ctx->sockfd = -1;
    ctx->is_running = false;
    ctx->on_message = callback;
    ctx->user_arg = user_arg;

    // 뮤텍스 초기화
    pthread_mutex_init( &ctx->conn_mutex, NULL );

    ctx->encrypt_fn = Packet_DefaultXor;
    ctx->decrypt_fn = Packet_DefaultXor;

    ctx->Connect = impl_Connect;
    ctx->Disconnect = impl_Disconnect;
    ctx->Send = impl_Send;
    ctx->SetStrategy = impl_SetStrategy;
    ctx->Destroy = impl_Destroy;

    return ctx;
}