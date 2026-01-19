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
        if( len < sizeof( ChatPacket ) ) return;

        ChatPacket* pkt = (ChatPacket*)body;
        printf( ">> [%s] %s\n", pkt->sender_id, pkt->message );
    }
}

int main( int argc, char* argv[] )
{
    // 사용자 ID는 실행 인자로 받음 (식별용)
    if( argc < 2 )
    {
        printf( "Usage: %s <UserID>\n", argv[0] );
        return 0;
    }

    char server_ip[32];
    int server_port;

    // -----------------------------------------------------------
    // 1. 서버 접속 정보 입력
    // -----------------------------------------------------------
    printf( "========================================\n" );
    printf( "   TCP Client Launcher\n" );
    printf( "========================================\n" );

    printf( "Server IP: " );
    if( scanf( "%31s", server_ip ) != 1 ) return -1;

    printf( "Server Port: " );
    if( scanf( "%d", &server_port ) != 1 ) return -1;

    // 입력 버퍼 비우기 (개행 문자 제거)
    getchar();

    printf( "----------------------------------------\n" );

    // 2. 내 정보 설정
    MyClientApp myApp;
    strncpy( myApp.my_id, argv[1], 32 );
    myApp.msg_sent_count = 0;

    // 3. 클라이언트 생성
    TcpClientContext* client = CreateTcpClientContext( OnServerMessage, &myApp );
    if( !client )
    {
        fprintf( stderr, "Client creation failed.\n" );
        return -1;
    }

    // 4. 서버 연결
    printf( "Connecting to %s:%d ...\n", server_ip, server_port );

    if( !client->Connect( client, server_ip, server_port ) )
    {
        fprintf( stderr, "Connection failed.\n" );
        client->Destroy( client );
        return -1;
    }

    // 5. 로그인 패킷 전송
    LoginReqPacket login;
    strncpy( login.user_id, myApp.my_id, 32 );
    strcpy( login.password, "1234" );
    login.version = 1;

    client->Send( client, "LOGIN", &login, sizeof( login ) );

    // 6. 채팅 시뮬레이션 루프
    printf( "[Info] Chat simulation started. Sending messages every 2 seconds.\n" );

    for( int i = 0; i < 100; ++i )
    {
        ChatPacket msg;
        strncpy( msg.sender_id, myApp.my_id, 32 );
        sprintf( msg.message, "Hello Packet %d from %s", i, myApp.my_id );
        msg.timestamp = (uint64_t)time( NULL );

        client->Send( client, "CHAT", &msg, sizeof( msg ) );

        myApp.msg_sent_count++;
        // printf( "[Sent] %s\n", msg.message ); // 로그가 너무 많으면 주석 처리

        sleep( 2 ); // 2초 대기
    }

    // 7. 종료
    client->Destroy( client );
    return 0;
}