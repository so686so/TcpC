/**
 * 파일명: examples/MainClient.c
 * 개요: TCP 클라이언트 구동 및 채팅 송수신
 * (실행 시 IP와 Port를 직접 입력받음)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "TcpClient.h"
#include "AppProtocol.h"

// --------------------------------------------------------------------------
// [ServiceContext] 클라이언트의 애플리케이션 상태
// --------------------------------------------------------------------------
typedef struct
{
    char my_id[32];
    int msg_sent_count;
} MyClientApp;


// --------------------------------------------------------------------------
// [Callback] 서버로부터 온 메시지 처리
// --------------------------------------------------------------------------
void OnServerMessage( TcpClientContext* ctx, void* user_arg,
                      const char* target, const char* body, int len )
{
    if( strncmp( target, "CHAT", TARGET_NAME_LEN ) == 0 )
    {
        ChatPacket* pkt = (ChatPacket*)body;
        printf( "\r>> [%s] %s\nMe: ", pkt->sender_id, pkt->message ); // 프롬프트 유지 꼼수
        fflush( stdout );
    }
}

int main( int argc, char* argv[] )
{
    if( argc < 2 ) { printf("Usage: %s <UserID>\n", argv[0]); return 0; }

    char server_ip[32];
    int server_port;

    printf( "Server IP: " ); scanf( "%31s", server_ip );
    printf( "Server Port: " ); scanf( "%d", &server_port );
    getchar(); // 버퍼 비우기

    MyClientApp myApp;
    strncpy( myApp.my_id, argv[1], 32 );

    TcpClientContext* client = CreateTcpClientContext( OnServerMessage, &myApp );

    // 1. 연결 시작 (백그라운드 스레드 가동)
    printf( "Starting Network Manager... (Target: %s:%d)\n", server_ip, server_port );
    client->Connect( client, server_ip, server_port );

    // 2. 로그인 패킷 전송 시도
    // (연결될 때까지 기다렸다가 보내거나, 그냥 보내고 실패하면 사용자가 다시 입력하게 함.
    //  여기서는 편의상 잠시 대기 후 자동 시도)
    sleep( 1 );
    LoginReqPacket login;
    strncpy( login.user_id, myApp.my_id, 32 );
    client->Send( client, "LOGIN", &login, sizeof( login ) );

    // 3. 채팅 입력 루프
    char input[128];
    printf( "=== Chat Room (Type 'q' to quit) ===\n" );

    while( 1 )
    {
        printf( "Me: " );
        if( fgets( input, sizeof( input ), stdin ) == NULL ) break;

        input[strcspn(input, "\n")] = 0; // 엔터 제거
        if( strcmp( input, "q" ) == 0 ) break;

        ChatPacket msg;
        strncpy( msg.sender_id, myApp.my_id, 32 );
        strncpy( msg.message, input, 127 );

        // 전송 시도
        int sent = client->Send( client, "CHAT", &msg, sizeof( msg ) );
        if( sent < 0 )
        {
            printf( "[System] Not connected to server. Reconnecting...\n" );
        }
    }

    client->Destroy( client );
    return 0;
}