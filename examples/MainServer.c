/**
 * 파일명: examples/MainServer.c
 *
 * 개요:
 * TCP 서버 구동 및 비즈니스 로직(로그인, 브로드캐스트) 처리.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "TcpServer.h"
#include "AppProtocol.h"

#define SERVER_PORT 3691

// 서버 종료를 위한 전역 플래그 (Signal Handler용)
volatile bool g_exit_flag = false;

void HandleSignal( int sig )
{
    printf( "\n[Main] Caught signal %d. Setting exit flag...\n", sig );
    g_exit_flag = true;
}

// --------------------------------------------------------------------------
// [ServiceContext] 서버의 애플리케이션 상태 데이터
// --------------------------------------------------------------------------
typedef struct
{
    int connected_users;
    long total_msgs_processed;
} MyServerApp;


// --------------------------------------------------------------------------
// [Callback] 클라이언트 메시지 처리 로직
// --------------------------------------------------------------------------
void OnClientMessage( TcpServerContext* ctx, int client_fd, void* user_arg,
                      const char* target, const char* body, int len )
{
    MyServerApp* app = (MyServerApp*)user_arg;

    // 1. 로그인 요청 처리
    if( strncmp( target, "LOGIN", TARGET_NAME_LEN ) == 0 )
    {
        if( len < sizeof( LoginReqPacket ) ) return;

        LoginReqPacket* pkt = (LoginReqPacket*)body;

        printf( "[ServerApp] Login Req: ID=%s, PWD=%s, Ver=%d (FD: %d)\n",
                pkt->user_id, pkt->password, pkt->version, client_fd );

        app->connected_users++;

        // (옵션) 환영 메시지 회신
        ChatPacket welcome;
        strcpy( welcome.sender_id, "SYSTEM" );
        sprintf( welcome.message, "Welcome %s! Users: %d", pkt->user_id, app->connected_users );

        ctx->Send( ctx, client_fd, "CHAT", &welcome, sizeof( welcome ) );
    }
    // 2. 채팅 메시지 처리 (브로드캐스트)
    else if( strncmp( target, "CHAT", TARGET_NAME_LEN ) == 0 )
    {
        if( len < sizeof( ChatPacket ) ) return;

        ChatPacket* pkt = (ChatPacket*)body;

        printf( "[ServerApp] Chat from %s: %s\n", pkt->sender_id, pkt->message );
        app->total_msgs_processed++;

        // 전체 클라이언트에게 재전송 (Echo Broadcast)
        ctx->Broadcast( ctx, "CHAT", pkt, sizeof( ChatPacket ) );

        printf( "[Server] Broadcast complete. Current Users: %d\n", ctx->GetClientCount( ctx ) );
    }
    else
    {
        printf( "[ServerApp] Unknown Target: %s\n", target );
    }
}

int main()
{
    signal( SIGINT, HandleSignal );

    MyServerApp app;
    TcpServerContext* server = CreateTcpServerContext( OnClientMessage, &app, 4 );

    if( server->Init( server, SERVER_PORT ) )
    {
        // Run 함수에 종료 플래그 포인터 전달
        // Ctrl+C 입력 시 g_exit_flag가 true가 되고, Run 루프가 종료됨.
        server->Run( server, &g_exit_flag );
    }

    // 루프 탈출 후 안전하게 자원 해제
    server->Destroy( server );
    printf( "[Main] Server application terminated safely.\n" );
    return 0;
}