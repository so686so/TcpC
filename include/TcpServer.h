/**
 * 파일명: include/TcpServer.h
 *
 * 개요:
 * 고성능 비동기 TCP 서버 컨텍스트 선언.
 * Epoll 기반의 IO 처리와 Worker Thread Pool 패턴,
 * Message Queue를 이용한 파이프라인 아키텍처를 제공한다.
 */

#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include "CommonDef.h" // 공통 타입 정의
#include "SafeQueue.h" // SafeQueue 구조체 및 함수 사용

#include <pthread.h>   // pthread_t, pthread_mutex_t
#include <sys/epoll.h> // epoll_event 구조체, epoll_* 함수 관련 타입

// --------------------------------------------------------------------------
// 1. 상수 및 설정 정의
// --------------------------------------------------------------------------

#define MAX_EPOLL_EVENTS 100  // 한 번의 epoll_wait에서 처리할 최대 이벤트 수
#define QUEUE_CAPACITY   1000 // 큐 최대 크기 (Backpressure 방지)

// --------------------------------------------------------------------------
// 2. 내부 태스크 구조체 및 전방 선언
// --------------------------------------------------------------------------

// 클라이언트 연결 관리용 노드 (내부 구현은 .c 파일에 은닉)
struct ClientNode;

/**
 * IO 스레드(Epoll)가 수신한 데이터를 워커 스레드로 넘길 때 사용하는 구조체
 */
typedef struct
{
    int   client_fd; // 데이터를 보낸 클라이언트 소켓
    char* data;      // 수신된 원본 데이터 (힙 할당됨, 워커가 해제해야 함)
    int   len;       // 데이터 길이
} ServerRecvTask;

/**
 * 워커 스레드가 처리를 마치고 송신 스레드로 넘길 때 사용하는 구조체
 */
typedef struct
{
    int  client_fd;    // 받을 대상 (-1이면 Broadcast, -2면 종료 신호)
    bool is_broadcast; // 브로드캐스트 여부

    char  target[TARGET_NAME_LEN]; // 패킷 타겟 코드
    char* body_data; // 전송할 바디 데이터 (힙 할당됨, 송신자가 해제해야 함)
    int   body_len;  // 바디 길이
} ServerSendTask;


// --------------------------------------------------------------------------
// 3. 콜백 함수 타입 정의
// --------------------------------------------------------------------------

/**
 * ## [OnServerMessageCallback]
 * 워커 스레드가 패킷을 파싱한 후 비즈니스 로직을 수행하기 위해 호출하는 콜백.
 *
 * ### [Params]
 * - srv_ctx     : 서버 컨텍스트 포인터
 * - client_fd   : 메시지를 보낸 클라이언트의 소켓 FD
 * - service_ctx : 사용자 정의 데이터 (ServiceContext 등)
 * - target      : 패킷 타겟 코드 (예: "LOGIN")
 * - body        : 복호화된 바디 데이터 포인터
 * - len         : 바디 길이
 */
typedef void ( *OnServerMessageCallback )( TcpServerContext* srv_ctx,
                                           int client_fd,
                                           void* service_ctx,
                                           const char* target,
                                           const char* body, int len );


// --------------------------------------------------------------------------
// 4. 서버 컨텍스트 구조체 정의
// --------------------------------------------------------------------------

struct TcpServerContext
{
    volatile bool is_running; // 서버 가동 상태 플래그

    // --- [Network Core] ---
    int listen_fd;
    int epoll_fd;
    struct epoll_event* events; // Epoll 이벤트 버퍼

    // --- [Thread Management] ---
    pthread_t worker_thread; // 작업 전담 스레드
    pthread_t sender_thread; // 송신 전담 스레드

    // --- [Data Pipeline (Queues)] ---
    SafeQueue* recv_queue; // Epoll  -> Worker (ServerRecvTask*)
    SafeQueue* send_queue; // Worker -> Sender (ServerSendTask*)

    // --- [Client Management] ---
    struct ClientNode* client_list_head;  // 연결된 클라이언트 리스트 헤드
    pthread_mutex_t    client_list_mutex; // 리스트 접근 동기화용 뮤텍스
    volatile int       current_client_count;

    // --- [User & Strategy] ---
    void*                   service_ctx; // on_message 콜백에 전달할 사용자가 구성한 서비스의 컨텍스트
    OnServerMessageCallback on_message;  // 수신 시 호출될 함수. 해당 함수에서 service_ctx 이용.

    EncryptFunc encrypt_fn; // 송신 패킷 암호화용
    DecryptFunc decrypt_fn; // 수신 패킷 복호화용

    /**
     * ##   서버를 초기화하고 포트를 바인딩한다. (Listen 시작)
     * #### 내부적으로 Epoll 인스턴스와 소켓을 생성한다.
     *
     * ### [Params]
     * - ctx  : 서버 컨텍스트
     * - port : 바인딩할 포트 번호
     *
     * ### [Return]
     * - true: 성공, false: 실패
     */
    bool ( *Init )( TcpServerContext* ctx, int port );

    /**
     * ##   서버 루프를 실행한다. (Blocking)
     * #### 워커 스레드와 송신 스레드를 생성하고, 메인 스레드는 Epoll 루프에 진입한다.
     *
     * ### [Params]
     * - ctx : 서버 컨텍스트
     * - exit_flag :
     *   외부에서 종료 신호를 줄 플래그 포인터 (NULL이면 사용 안 함)
     *   이 값이 true가 되면 루프를 탈출한다.
     */
    void ( *Run )( TcpServerContext* ctx, volatile bool* exit_flag );

    /**
     * ##   특정 클라이언트에게 데이터를 전송하도록 큐에 작업을 등록한다.
     * #### 실제 전송은 Sender 스레드에서 비동기로 처리된다.
     *
     * ### [Params]
     * - ctx       : 서버 컨텍스트
     * - client_fd : 수신할 클라이언트 소켓 FD
     * - target    : 패킷 타겟 문자열
     * - body      : 전송할 데이터 구조체 또는 버퍼
     * - len       : 데이터 길이
     *
     * ### [Return]
     * - true: 큐 등록 성공, false: 실패 (큐 가득 참 등)
     */
    bool ( *Send )( TcpServerContext* ctx, int client_fd, const char* target, void* body, int len );

    /**
     * ##   현재 접속된 모든 클라이언트에게 데이터를 전송한다. (Broadcast)
     * #### 내부 관리되는 클라이언트 리스트를 순회하며 전송한다.
     *
     * ### [Params]
     * - ctx    : 서버 컨텍스트
     * - target : 패킷 타겟 문자열
     * - body   : 전송할 데이터
     * - len    : 데이터 길이
     *
     * ### [Return]
     * - true: 큐 등록 성공, false: 실패
     */
    bool ( *Broadcast )( TcpServerContext* ctx, const char* target, void* body, int len );

    /**
     * ## 암호화/복호화 전략을 설정한다.
     *
     * ### [Params]
     * - ctx : 서버 컨텍스트
     * - enc : 암호화 함수 포인터
     * - dec : 복호화 함수 포인터
     */
    void ( *SetStrategy )( TcpServerContext* ctx, EncryptFunc enc, DecryptFunc dec );

    /**
     * ##   서버를 종료하고 자원을 해제한다.
     * #### 실행 중인 모든 스레드에 종료 신호(Poison Pill)를 보내고 대기한다.
     *
     * ### [Params]
     * - ctx : 서버 컨텍스트
     */
    void ( *Destroy )( TcpServerContext* ctx );

    /**
     * ## 현재 접속 중인 클라이언트(FD)의 개수를 반환한다.
     */
    int ( *GetClientCount )( TcpServerContext* ctx );
};


// --------------------------------------------------------------------------
// 5. 생성자 선언
// --------------------------------------------------------------------------

/**
 * ## TcpServerContext 객체를 생성한다.
 * #### 내부 큐와 스레드 배열을 할당하고 멤버 변수를 초기화한다.
 *
 * ### [Param]
 * - callback     : 메시지 수신 시 처리할 콜백 함수
 * - service_ctx  : 콜백에 전달될 사용자 데이터 컨텍스트
 *
 * ### [Return]
 * - 생성된 객체 포인터 (실패 시 NULL)
 */
TcpServerContext* CreateTcpServerContext( OnServerMessageCallback callback, void* service_ctx );

#endif // TCP_SERVER_H