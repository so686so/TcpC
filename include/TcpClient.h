/**
 * 파일명: include/TcpClient.h
 *
 * 개요:
 * TCP 클라이언트의 연결, 송수신, 스레드 관리 및 콜백 처리를 담당하는 컨텍스트 선언
 */

#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include "CommonDef.h" // CommonDef의 전방 선언 및 타입 사용

#include <pthread.h>   // pthread_t (스레드 핸들), pthread_mutex_t (뮤텍스)

// --------------------------------------------------------------------------
// 1. 콜백 함수 타입 정의
// --------------------------------------------------------------------------

/**
 * ## 서버로부터 패킷(Body)을 수신했을 때 호출되는 콜백 함수.
 *
 * ### [Params]
 * - client_ctx  : 이벤트를 발생시킨 클라이언트 컨텍스트
 * - service_ctx : 사용자가 등록한 외부 컨텍스트 (ServiceContext 등)
 * - target      : 패킷에 명시된 타겟 코드 (예: "LOGIN")
 * - body        : 복호화가 완료된 바디 데이터 포인터
 * - len         : 바디 데이터 길이
 */
typedef void ( *OnMessageCallback )
(
    // contexts
    TcpClientContext* client_ctx, void* service_ctx,

    // recv data
    const char* target, const char* body, int len
);


// --------------------------------------------------------------------------
// 2. 클라이언트 컨텍스트 구조체 정의
// --------------------------------------------------------------------------

struct TcpClientContext
{
    int sockfd;                 // 연결 안됨: -1, 연결됨: >=0
    pthread_mutex_t conn_mutex; // sockfd 접근 보호용 뮤텍스

    volatile bool is_running; // 클라이언트 동작 여부
    pthread_t network_thread; // 연결 관리 및 수신 담당 스레드

    // 서버 정보 (연결 후 저장용)
    char server_ip[32];
    int  server_port;

    // 외부 연동 데이터
    void*             service_ctx; // 콜백에 전달할 사용자가 구성한 서비스의 컨텍스트
    OnMessageCallback on_message;  // 수신 시 호출될 함수. 해당 함수에서 service_ctx 이용.

    // 암호화/복호화 전략 (Strategy Pattern)
    EncryptFunc encrypt_fn; // 송신 시 사용하는 암호화 함수
    DecryptFunc decrypt_fn; // 수신 시 사용하는 복호화 함수

    /**
     * ##   서버에 연결을 시도한다. (Blocking)
     * #### 내부적으로 스레드를 띄워 주기적으로 연결을 시도한다.
     *
     * ### [Params]
     * - ip   : 서버 IP 주소
     * - port : 서버 포트 번호
     *
     * ### [Return]
     * - true: 성공, false: 실패
     */
    bool ( *Connect )( TcpClientContext* ctx, const char* ip, int port );

    /**
     * ## 현재 서버와 연결(핸드셰이크 완료)되어 있는지 확인한다. (Thread-Safe)
     *
     * ### [Return]
     * - true: 연결됨 (전송 가능)
     * - false: 연결 끊김 또는 연결 시도 중
     */
    bool ( *IsConnected )( TcpClientContext* ctx );

    /**
     * ##   연결을 종료하고 소켓을 닫는다.
     * #### 수신 스레드도 종료된다.
     */
    void ( *Disconnect )( TcpClientContext* ctx );

    /**
     * ##   데이터를 패킷으로 포장하여 전송한다. (Thread-Safe by Socket)
     * #### 내부적으로 Serialize -> Encrypt -> Send 과정을 수행한다.
     *
     * ### [Params]
     * - target   : 패킷 식별 문자열 (예: "CHAT", "LOGIN")
     * - body     : 전송할 구조체 또는 데이터 포인터
     * - body_len : body 데이터의 길이
     *
     * ### [Return]
     * - 전송된 바이트 수 (-1: 에러)
     */
    int ( *Send )( TcpClientContext* ctx, const char* target, void* body, int body_len );

    /**
     * ##   암호화/복호화 함수를 동적으로 변경한다.
     * #### 핸드셰이크 이후 보안 수준을 격상할 때 사용한다.
     *
     * ### [Params]
     * - enc : 암호화 함수 포인터 (NULL 허용)
     * - dec : 복호화 함수 포인터 (NULL 허용)
     */
    void ( *SetStrategy )( TcpClientContext* ctx, EncryptFunc enc, DecryptFunc dec );

    /**
     * ##   TcpClientContext를 파괴하고 메모리를 해제한다.
     * #### 내부적으로 Disconnect를 호출하여 스레드를 정리한다.
     */
    void ( *Destroy )( TcpClientContext* ctx );
};


// --------------------------------------------------------------------------
// 3. 생성자 선언
// --------------------------------------------------------------------------

/**
 * ##   TcpClientContext 객체를 생성하고 초기화한다.
 * #### 콜백 함수와 사용자 인자를 필수적으로 바인딩한다.
 *
 * ### [Params]
 * - callback    : 메시지 수신 시 호출될 함수
 * - service_ctx : 콜백에 전달될 사용자 데이터 컨텍스트
 *
 * ### [Return]
 * - 생성된 객체 포인터 (실패 시 NULL)
 */
TcpClientContext* CreateTcpClientContext( OnMessageCallback callback, void* service_ctx );

#endif // TCP_CLIENT_H