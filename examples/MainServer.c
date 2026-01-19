/**
 * 파일명: examples/MainServer.c
 *
 * 개요:
 * TcpServer 라이브러리를 활용한 채팅 서버 예제 애플리케이션.
 *
 * [주요 기능]
 * 1. 서버 초기화 및 포트 바인딩 (실패 시 종료)
 * 2. ServiceContext를 활용한 애플리케이션 상태 관리
 * 3. 비즈니스 로직(로그인, 채팅) 처리 및 브로드캐스트
 * 4. 시그널 핸들링을 통한 안전한 종료 (Graceful Shutdown)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "TcpServer.h"
#include "AppProtocol.h" // 비즈니스 프로토콜 (LOGIN, CHAT 구조체 등)

// --------------------------------------------------------------------------
// 1. 전역 설정 및 변수
// --------------------------------------------------------------------------

#define DEFAULT_PORT 3691

// 서버 종료를 위한 전역 플래그 (Signal Handler에서 변경)
volatile bool g_exit_flag = false;

// --------------------------------------------------------------------------
// 2. ServiceContext 정의
// --------------------------------------------------------------------------

/**
 * ## [ChatServiceContext]
 *
 * 서버의 비즈니스 로직에서 유지해야 할 상태 데이터.
 * 라이브러리(TcpServer)는 네트워크 연결만 관리하고,
 * 실제 서비스 데이터(총 메시지 수, 게임 방 상태 등)는 이 구조체에 담아 관리한다.
 *
 * 이 포인터는 콜백 함수의 'service_ctx' 인자로 전달된다.
 */
typedef struct
{
    long total_msgs_processed;  // 서버 가동 후 처리된 총 메시지 수
    // 필요하다면 DB 핸들, 게임 룸 리스트 등을 여기에 추가
} ChatServiceContext;


// --------------------------------------------------------------------------
// 3. 함수 전방 선언 (Forward Declarations)
// --------------------------------------------------------------------------

void HandleSignal( int sig );

void OnClientMessage( TcpServerContext* ctx, int client_fd, void* service_ctx,
                      const char* target, const char* body, int len );


// --------------------------------------------------------------------------
// 4. 메인 함수 (Entry Point)
// --------------------------------------------------------------------------

int main( int argc, char* argv[] )
{
    // 1. 시그널 핸들러 등록 (Ctrl+C 처리)
    signal( SIGINT, HandleSignal );

    // 2. 포트 설정 (입력값이 없으면 기본값 사용)
    int port = DEFAULT_PORT;
    if( argc >= 2 )
    {
        port = atoi( argv[1] );
    }

    printf( "========================================\n" );
    printf( "   High-Performance TCP Chat Server\n" );
    printf( "========================================\n" );
    printf( "[Info] Server Port: %d\n", port );

    // 3. 서비스 컨텍스트 초기화
    ChatServiceContext my_service_ctx = { 0 };

    // 4. 서버 객체 생성
    // - OnClientMessage: 메시지 수신 시 호출될 비즈니스 로직
    // - &my_service_ctx: 콜백에 전달할 상태 데이터
    TcpServerContext* server = CreateTcpServerContext( OnClientMessage, &my_service_ctx );

    if( !server )
    {
        fprintf( stderr, "[Error] Failed to create server context.\n" );
        return -1;
    }

    // 5. 서버 초기화 및 포트 바인딩
    if( !server->Init( server, port ) )
    {
        fprintf( stderr, "[Error] Failed to bind port %d. (Is it already in use?)\n", port );
        server->Destroy( server );
        return -1;
    }

    // 6. 서버 루프 실행 (Blocking)
    // g_exit_flag가 true가 될 때까지 내부 루프를 돕니다.
    server->Run( server, &g_exit_flag );

    // 7. 자원 해제 및 종료
    server->Destroy( server );

    printf( "[Main] Server application terminated safely. Total Msgs: %ld\n",
        my_service_ctx.total_msgs_processed );

    return 0;
}


// --------------------------------------------------------------------------
// 5. 함수 구현부
// --------------------------------------------------------------------------

/**
 * ## 시그널 핸들러
 * Ctrl+C (SIGINT) 입력 시 종료 플래그를 설정하여 서버 루프를 안전하게 멈춘다.
 */
void HandleSignal( int sig )
{
    printf( "\n[Main] Caught signal %d. Initiating graceful shutdown...\n", sig );
    g_exit_flag = true;
}

/**
 * ## [비즈니스 로직] 클라이언트 메시지 처리
 *
 * 워커 스레드에서 호출되며, 수신된 패킷을 분석하여 비즈니스 로직을 수행한다.
 * 단일 워커 스레드 모델이므로 별도의 Mutex 없이 service_ctx에 안전하게 접근 가능하다.
 *
 * @param ctx         서버 라이브러리 컨텍스트 (Send, Broadcast 등 기능 제공)
 * @param client_fd   메시지를 보낸 클라이언트의 소켓 FD
 * @param service_ctx 사용자가 등록한 애플리케이션 데이터 (ChatServiceContext)
 * @param target      패킷 타겟 코드 (AppProtocol.h 의 상수 사용 권장)
 * @param body        복호화된 바디 데이터 포인터
 * @param len         바디 데이터 길이
 */
void OnClientMessage( TcpServerContext* ctx, int client_fd, void* service_ctx,
                      const char* target, const char* body, int len )
{
    // void* 로 받은 컨텍스트를 원래 타입으로 캐스팅하여 사용
    ChatServiceContext* app_data = (ChatServiceContext*)service_ctx;

    // -----------------------------------------------------------
    // Case 1: 로그인 요청 (TARGET_APP_LOGIN)
    // -----------------------------------------------------------
    if( strncmp( target, TARGET_APP_LOGIN, TARGET_NAME_LEN ) == 0 )
    {
        // 구조체 크기만큼 데이터가 왔는지 검증
        if( len < sizeof( LoginReqPacket ) ) return;

        LoginReqPacket* pkt = (LoginReqPacket*)body;

        // 라이브러리 기능을 이용해 현재 접속자 수 조회
        int current_users = ctx->GetClientCount( ctx );

        printf( "[Login] User: %s (FD: %d) | Ver: %d | Current Users: %d\n",
            pkt->user_id, client_fd, pkt->version, current_users );

        // 환영 메시지 전송 (유니캐스트)
        ChatPacket welcome_msg;
        strcpy( welcome_msg.sender_id, "SYSTEM" );
        sprintf( welcome_msg.message, "Welcome %s! Users online: %d", pkt->user_id, current_users );
        welcome_msg.timestamp = 0; // 예시

        // 특정 클라이언트에게 전송
        ctx->Send( ctx, client_fd, TARGET_APP_CHAT, &welcome_msg, sizeof( welcome_msg ) );
    }
    // -----------------------------------------------------------
    // Case 2: 채팅 메시지 (TARGET_APP_CHAT)
    // -----------------------------------------------------------
    else if( strncmp( target, TARGET_APP_CHAT, TARGET_NAME_LEN ) == 0 )
    {
        if( len < sizeof( ChatPacket ) ) return;

        ChatPacket* pkt = (ChatPacket*)body;

        printf( "[Chat] [%s]: %s\n", pkt->sender_id, pkt->message );

        // 애플리케이션 통계 업데이트
        app_data->total_msgs_processed++;

        // 접속한 모든 클라이언트에게 재전송 (Echo Broadcast)
        // 송신 스레드에게 작업을 위임하므로 여기서 블로킹되지 않음
        ctx->Broadcast( ctx, TARGET_APP_CHAT, pkt, sizeof( ChatPacket ) );
    }
    // -----------------------------------------------------------
    // Case 3: 알 수 없는 패킷
    // -----------------------------------------------------------
    else
    {
        printf( "[Warning] Unknown Target Received: %s\n", target );
    }
}