/**
 * 파일명: examples/MainClient.c
 *
 * 개요:
 * TcpClient 라이브러리를 활용한 채팅 클라이언트 예제.
 *
 * [주요 기능]
 * 1. 서버 접속 및 백그라운드 재연결 관리
 * 2. 로그인 패킷 전송 (애플리케이션 프로토콜)
 * 3. 사용자 입력 대기 및 채팅 메시지 전송
 * 4. 수신된 메시지 실시간 출력 (콜백)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "TcpClient.h"
#include "AppProtocol.h" // 비즈니스 프로토콜 (LOGIN, CHAT 구조체 및 상수)

// --------------------------------------------------------------------------
// 1. 전역 설정 및 상수
// --------------------------------------------------------------------------

#define DEFAULT_SERVER_IP   "127.0.0.1"
#define DEFAULT_SERVER_PORT 3691


// --------------------------------------------------------------------------
// 2. ServiceContext 정의
// --------------------------------------------------------------------------

/**
 * ## [ChatClientContext]
 * 클라이언트 애플리케이션이 유지해야 할 상태 데이터.
 * (예: 내 아이디, 보낸 메시지 카운트, 현재 접속 상태 등)
 *
 * 이 데이터는 CreateTcpClientContext 시 등록되며,
 * OnServerMessage 콜백 호출 시 인자로 전달된다.
 */
typedef struct
{
    char my_user_id[32]; // 로그인한 내 아이디
    int  msg_sent_count; // 통계용: 내가 보낸 메시지 수
} ChatClientContext;


// --------------------------------------------------------------------------
// 3. 함수 전방 선언
// --------------------------------------------------------------------------

// 서버로부터 메시지가 도착했을 때 호출되는 콜백
void OnServerMessage( TcpClientContext* ctx, void* service_ctx,
                      const char* target, const char* body, int len );


// --------------------------------------------------------------------------
// 4. 메인 함수
// --------------------------------------------------------------------------

int main( int argc, char* argv[] )
{
    // 1. 사용자 ID 입력 확인 (필수)
    if( argc < 2 )
    {
        printf( "Usage: %s <UserID> [ServerIP] [Port]\n", argv[0] );
        return 0;
    }

    // 2. 서버 접속 정보 설정 (인자 없으면 기본값)
    char server_ip[32];
    int  server_port = DEFAULT_SERVER_PORT;

    // Get IP
    strncpy( server_ip, ( argc >= 3 ) ? argv[2] : DEFAULT_SERVER_IP, 32 );

    // Get Port
    if( argc >= 4 )
        server_port = atoi( argv[3] );

    printf( "========================================\n" );
    printf( "   TCP Chat Client (User: %s)\n", argv[1] );
    printf( "========================================\n" );

    // 3. 클라이언트 컨텍스트(상태) 초기화
    ChatClientContext my_app;
    memset( &my_app, 0, sizeof( ChatClientContext ) );
    strncpy( my_app.my_user_id, argv[1], 32 );

    // 4. TCP 클라이언트 객체 생성
    // - OnServerMessage: 수신 콜백
    // - &my_app: 콜백에 전달될 내 상태 데이터
    TcpClientContext* client = CreateTcpClientContext( OnServerMessage, &my_app );

    if( !client )
    {
        fprintf( stderr, "[Error] Failed to create client context.\n" );
        return -1;
    }

    // 5. 서버 연결 시도 (비동기 스레드 시작)
    // Connect 함수는 즉시 리턴되며, 내부 스레드가 백그라운드에서 연결 및 핸드셰이크를 수행함.
    printf( "[System] Connecting to %s:%d ...\n", server_ip, server_port );
    client->Connect( client, server_ip, server_port );

    int wait_count = 0;
    while( !client->IsConnected( client ) )
    {
        // 600초 이상 연결 안 되면 타임아웃 처리 등도 가능
        if( wait_count++ > 6000 ) { // 600초
            printf( "\n[Error] Connection Timeout!\n" );
            client->Destroy( client );
            return -1;
        }

        usleep( 100000 ); // 100ms 대기
        printf( "." );
        fflush( stdout );
    }
    printf( "\n[System] Connected Successfully!\n" );

    // 6. 로그인 패킷 전송
    LoginReqPacket login_pkt;
    memset( &login_pkt, 0, sizeof( LoginReqPacket ) );
    strncpy( login_pkt.user_id, my_app.my_user_id, 32 );
    snprintf( login_pkt.password, 48, "%s_%d", my_app.my_user_id, server_port );
    login_pkt.version = 1;

    // Send 함수는 내부적으로 직렬화 -> 암호화 -> 전송을 수행함
    if( client->Send( client, TARGET_APP_LOGIN, &login_pkt, sizeof( LoginReqPacket ) ) > 0 )
    {
        printf( "[System] Login packet sent.\n" );
    }
    else
    {
        printf( "[System] Failed to send login. (Not connected yet?)\n" );
    }

    // 7. 채팅 입력 루프
    char input_buf[128];
    printf( "=== Chat Room (Type 'q' to quit) ===\n" );

    while( 1 )
    {
        // 입력 프롬프트
        printf( "Me: " );

        if( fgets( input_buf, sizeof( input_buf ), stdin ) == NULL ) break;

        // 개행 문자 제거
        input_buf[strcspn( input_buf, "\n" )] = 0;

        // 종료 커맨드 체크
        if( strcmp( input_buf, "q" ) == 0 ) break;
        if( strlen( input_buf )      == 0 ) continue; // 빈 줄 무시

        // 채팅 패킷 구성
        ChatPacket msg_pkt;
        memset( &msg_pkt, 0, sizeof( ChatPacket ) );
        strncpy( msg_pkt.sender_id, my_app.my_user_id, 32 );
        strncpy( msg_pkt.message, input_buf, 127 );
        msg_pkt.timestamp = (uint64_t)time( NULL );

        // 전송
        int sent_bytes = client->Send( client, TARGET_APP_CHAT, &msg_pkt, sizeof( ChatPacket ) );

        if( sent_bytes < 0 )
        {
            // 재연결은 라이브러리가 자동으로 시도하지만, 현재 메시지는 실패했음을 알림
            printf( "[System] Connection lost. Message dropped (Auto-reconnecting...)\n" );
        }
        else
        {
            my_app.msg_sent_count++;
        }
    }

    // 8. 종료 및 자원 해제
    printf( "[System] Client shutting down...\n" );
    client->Destroy( client );

    return 0;
}


// --------------------------------------------------------------------------
// 5. 함수 구현부
// --------------------------------------------------------------------------

/**
 * ## [수신 콜백] 서버 메시지 처리
 *
 * 네트워크 스레드에서 호출되며, 서버가 보낸 패킷을 처리한다.
 *
 * @param ctx         클라이언트 라이브러리 컨텍스트
 * @param service_ctx 사용자가 등록한 애플리케이션 데이터 (ChatClientContext)
 * @param target      패킷 타겟 코드
 * @param body        복호화된 바디 데이터 포인터
 * @param len         바디 데이터 길이
 */
void OnServerMessage( TcpClientContext* ctx, void* service_ctx,
                      const char* target, const char* body, int len )
{
    // 내 상태 데이터 (필요 시 사용)
    // ChatClientContext* my_app = (ChatClientContext*)service_ctx;

    // -----------------------------------------------------------
    // Case 1: 채팅 메시지 수신 (TARGET_APP_CHAT)
    // -----------------------------------------------------------
    if( strncmp( target, TARGET_APP_CHAT, TARGET_NAME_LEN ) == 0 )
    {
        // 데이터 크기 검증
        if( len < sizeof( ChatPacket ) ) return;

        ChatPacket* pkt = (ChatPacket*)body;

        // UI 출력 트릭:
        // 사용자가 입력 중이던 "Me: " 프롬프트를 덮어쓰지 않기 위해
        // 캐리지 리턴(\r)을 사용하여 줄을 지우고 출력 후 다시 프롬프트 표시
        printf( "\r>> [%s] %s\nMe: ", pkt->sender_id, pkt->message );
        fflush( stdout );
    }
    // -----------------------------------------------------------
    // Case 2: 기타 패킷 처리 (필요 시 추가)
    // -----------------------------------------------------------
    else
    {
        // printf( "\r[Info] Recv Unknown Target: %s\nMe: ", target );
        // fflush( stdout );
    }
}