/**
 * 파일명: src/TcpClient.c
 * 개요: TcpClient.h 에 선언된 클라이언트 기능 구현부
 */

#include "TcpClient.h"
#include "PacketUtils.h" // 패킷 직렬화/역직렬화 함수 사용

#include <stdio.h>       // printf (로그 출력)
#include <stdlib.h>      // malloc, free (버퍼 및 컨텍스트 할당)
#include <unistd.h>      // close, sleep, shutdown (시스템 콜)
#include <string.h>      // strncpy, memset, strncmp (문자열 및 메모리 조작)
#include <arpa/inet.h>   // inet_pton, htons, ntohl (주소 변환 및 바이트 오더링)
#include <sys/socket.h>  // socket, connect, recv, send, sockaddr 구조체
#include <errno.h>       // errno (에러 코드 확인)

// --------------------------------------------------------------------------
// 1. 내부 헬퍼 함수 (Low-Level IO)
// --------------------------------------------------------------------------

static int RecvExact( int fd, char* buf, int len )
{
    int total_read = 0;
    while( total_read < len )
    {
        int received = recv( fd, buf + total_read, len - total_read, 0 );
        if( received <= 0 ) return received; // 0: Close, -1: Error
        total_read += received;
    }
    return total_read;
}

// --------------------------------------------------------------------------
// 2. 연결 상태 관리 함수 (Connection Management)
// --------------------------------------------------------------------------

/**
 * ## 연결 정보를 초기화한다. (에러 발생 혹은 종료 시 호출)
 * - 소켓 Close
 * - FD를 -1로 설정
 * - 암호화 전략을 기본값(혹은 평문)으로 초기화
 */
static void ResetConnection( TcpClientContext* ctx )
{
    pthread_mutex_lock( &ctx->conn_mutex );

    if( ctx->sockfd != -1 )
    {
        // printf( "[TcpClient] Resetting connection (FD: %d)...\n", ctx->sockfd );
        close( ctx->sockfd );
        ctx->sockfd = -1;
    }

    // 재연결 시 평문 핸드셰이크를 위해 전략 초기화
    // '서버' 생성자 초기값과 동일하게 맞춤
    ctx->encrypt_fn = Packet_DefaultXor;
    ctx->decrypt_fn = Packet_DefaultXor;

    pthread_mutex_unlock( &ctx->conn_mutex );
}

/**
 * ## 소켓 생성 및 TCP 연결 시도 (Handshake 전 단계)
 * Return: 연결된 소켓 FD (실패 시 -1)
 */
static int TryConnect( const char* ip, int port )
{
    int sock = socket( AF_INET, SOCK_STREAM, 0 );
    if( sock < 0 )
        return -1;

    struct sockaddr_in serv_addr;
    memset( &serv_addr, 0, sizeof( serv_addr ) );

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons( port );

    if( inet_pton( AF_INET, ip, &serv_addr.sin_addr ) <= 0 )
    {
        close( sock );
        return -1;
    }

    // 타임아웃 설정을 추가할 수도 있음 (여기선 Blocking Connect)
    if( connect( sock, (struct sockaddr*)&serv_addr, sizeof( serv_addr ) ) < 0 )
    {
        close( sock );
        return -1;
    }

    return sock;
}

/**
 * ## 핸드셰이크 처리 (보안 전략 수신 및 설정)
 * Return: true(성공), false(실패)
 */
static bool TryHandshake( TcpClientContext* ctx, int sock, char* buffer )
{
    const int header_size = sizeof( PacketHeader );

    // 1. 헤더 수신
    if( RecvExact( sock, buffer, header_size ) <= 0 )
        return false;

    PacketHeader* header    = (PacketHeader*)buffer;
    uint32_t      total_len = ntohl( header->total_len );

    // 2. 길이 검사
    if( total_len > DEFAULT_BUF_SIZE || total_len < header_size )
        return false;

    // 3. 바디 수신
    int body_len = total_len - header_size;
    if( body_len > 0 )
    {
        if( RecvExact( sock, buffer + header_size, body_len ) <= 0 )
            return false;
    }

    // 4. 파싱 (평문)
    char  target_buf[TARGET_NAME_LEN];
    char* body_ptr        = NULL;
    int   parsed_body_len = 0;

    PacketResult result
        = Packet_Parse( buffer, total_len, NULL, target_buf, &body_ptr, &parsed_body_len );

    if( result != PKT_SUCCESS )
        return false;

    // 5. 검증 및 설정
    if( strncmp( target_buf, TARGET_SEC_STRATEGY, TARGET_NAME_LEN ) != 0 )
        return false;

    if( parsed_body_len < sizeof( SecurityStrategyBody ) )
        return false;

    SecurityStrategyBody* strategy_pkt = (SecurityStrategyBody*)body_ptr;
    int code = strategy_pkt->strategy_code;

    // 전략 적용
    ctx->SetStrategy( ctx, Packet_GetEncryptFunc( code ), Packet_GetDecryptFunc( code ) );

    printf( "[TcpClient] Handshake Success. Strategy: %d\n", code );
    return true;
}

// --------------------------------------------------------------------------
// 네트워크 관리 스레드 (재연결 + 수신)
// --------------------------------------------------------------------------

static void* NetworkManagerThread( void* arg )
{
    // Get context
    TcpClientContext* ctx = (TcpClientContext*)arg;

    // Set buffer memory
    char* recv_buf = (char*)malloc( DEFAULT_BUF_SIZE );

    // printf( "[TcpClient] Network Manager Started. Target: %s:%d\n",
    //     ctx->server_ip, ctx->server_port );

    // Get Header size
    const int header_size = sizeof( PacketHeader );

    while( ctx->is_running )
    {
        // ---------------------------------------------------------
        // [State Check] 현재 연결 상태 확인
        // ---------------------------------------------------------
        int curr_fd = -1;
        pthread_mutex_lock( &ctx->conn_mutex );
        curr_fd = ctx->sockfd;
        pthread_mutex_unlock( &ctx->conn_mutex );

        // ---------------------------------------------------------
        // Case 1: 연결이 끊어진 상태 (Disconnected)
        // -> 재연결 시도
        // ---------------------------------------------------------
        if( curr_fd == -1 )
        {
            // 1. TCP 연결
            int sock = TryConnect( ctx->server_ip, ctx->server_port );
            if( sock < 0 ){
                sleep( 1 ); // 1초 뒤 재시도
                continue;
            }

            // 2. 애플리케이션 핸드셰이크

            // 성공: 소켓 등록 (State -> Connected)
            if( TryHandshake( ctx, sock, recv_buf ) )
            {
                pthread_mutex_lock( &ctx->conn_mutex );
                ctx->sockfd = sock;
                pthread_mutex_unlock( &ctx->conn_mutex );
            }
            // 실패: 즉시 정리 후 재시도
            else
            {
                close( sock );
                sleep( 1 );
            }
            continue;
        }

        // ---------------------------------------------------------
        // Case 2: 연결된 상태 (Connected) -> 메시지 수신 루프
        // ---------------------------------------------------------

        // A. 헤더 읽기
        if( RecvExact( curr_fd, recv_buf, header_size ) <= 0 )
        {
            ResetConnection( ctx ); // 에러 발생 시 초기화
            continue;
        }

        PacketHeader* header    = (PacketHeader*)recv_buf;
        uint32_t      total_len = ntohl( header->total_len );

        // B. 유효성 검사
        if( total_len > DEFAULT_BUF_SIZE || total_len < header_size )
        {
            printf( "[TcpClient] Invalid Packet Len: %u\n", total_len );
            ResetConnection( ctx );
            continue;
        }

        // C. 바디 읽기
        int body_len = total_len - header_size;
        if( body_len > 0 )
        {
            if( RecvExact( curr_fd, recv_buf + header_size, body_len ) <= 0 )
            {
                ResetConnection( ctx );
                continue;
            }
        }

        // D. 파싱 및 콜백
        char  target_buf[TARGET_NAME_LEN];
        char* body_ptr   = NULL;
        int   parsed_len = 0;

        if( Packet_Parse( recv_buf, total_len, ctx->decrypt_fn, target_buf, &body_ptr, &parsed_len ) == PKT_SUCCESS )
        {
            if( ctx->on_message )
            {
                ctx->on_message( ctx, ctx->service_ctx, target_buf, body_ptr, parsed_len );
            }
        }
        else
        {
             // 파싱 실패(체크섬 등)는 연결을 끊을 수도 있고, 로그만 남길 수도 있음.
             // 여기선 안전을 위해 재연결
             ResetConnection( ctx );
        }

    } // while(is_running)

    // 스레드 종료 시 자원 정리
    if( recv_buf )
        free( recv_buf );

    ResetConnection( ctx ); // 마지막으로 확실하게 정리

    return NULL;
}

// --------------------------------------------------------------------------
// 멤버 함수 구현
// --------------------------------------------------------------------------

static bool impl_Connect( TcpClientContext* ctx, const char* ip, int port )
{
    if( !ctx )
        return false;

    strncpy( ctx->server_ip, ip, sizeof( ctx->server_ip ) - 1 );

    ctx->server_port = port;
    ctx->is_running  = true;

    // 스레드 시작
    if( pthread_create( &ctx->network_thread, NULL, NetworkManagerThread, ctx ) != 0 ){
        return false;
    }
    return true;
}

static bool impl_IsConnected( TcpClientContext* ctx )
{
    if( !ctx )
        return false;

    bool connected = false;

    pthread_mutex_lock( &ctx->conn_mutex );
    {
        // NetworkManagerThread 로직상,
        // 핸드셰이크가 성공해야 sockfd가 -1이 아닌 값으로 설정됨.
        if( ctx->is_running && ctx->sockfd != -1 )
        {
            connected = true;
        }
    }
    pthread_mutex_unlock( &ctx->conn_mutex );

    return connected;
}

static void impl_Disconnect( TcpClientContext* ctx )
{
    if( !ctx )
        return;

    ctx->is_running = false;

    // 소켓 강제 종료로 recv 블로킹 해제 유도
    pthread_mutex_lock( &ctx->conn_mutex );
    if( ctx->sockfd != -1 )
    {
        shutdown( ctx->sockfd, SHUT_RDWR );
        // close는 ResetConnection이나 스레드 종료부에서 처리됨
    }
    pthread_mutex_unlock( &ctx->conn_mutex );

    pthread_join( ctx->network_thread, NULL );
}

static int impl_Send( TcpClientContext* ctx, const char* target, void* body, int len )
{
    if( !ctx || !ctx->is_running )
        return -1;

    int fd = -1;
    pthread_mutex_lock( &ctx->conn_mutex );
    fd = ctx->sockfd;
    pthread_mutex_unlock( &ctx->conn_mutex );

    if( fd == -1 )
        return -1; // 연결 안됨

    char* send_buf = (char*)malloc( DEFAULT_BUF_SIZE );
    int   pkt_len  = Packet_Serialize( send_buf, DEFAULT_BUF_SIZE, target, body, len, ctx->encrypt_fn );

    int sent = -1;
    if( pkt_len > 0 ){
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

    if( ctx->is_running ){
        ctx->Disconnect( ctx );
    }

    pthread_mutex_destroy( &ctx->conn_mutex );
    free( ctx );

    // printf( "[TcpClient] Context destroyed.\n" );
}


// --------------------------------------------------------------------------
// 생성자 구현
// --------------------------------------------------------------------------

TcpClientContext* CreateTcpClientContext( OnMessageCallback callback, void* service_ctx )
{
    TcpClientContext* ctx = (TcpClientContext*)malloc( sizeof( TcpClientContext ) );

    if( ctx == NULL )
        return NULL;

    memset( ctx, 0, sizeof( TcpClientContext ) );

    ctx->sockfd      = -1;
    ctx->is_running  = false;
    ctx->on_message  = callback;
    ctx->service_ctx = service_ctx;

    // 뮤텍스 초기화
    pthread_mutex_init( &ctx->conn_mutex, NULL );

    ctx->encrypt_fn = Packet_DefaultXor;
    ctx->decrypt_fn = Packet_DefaultXor;

    ctx->Connect     = impl_Connect;
    ctx->IsConnected = impl_IsConnected;
    ctx->Disconnect  = impl_Disconnect;
    ctx->Send        = impl_Send;
    ctx->SetStrategy = impl_SetStrategy;
    ctx->Destroy     = impl_Destroy;

    return ctx;
}