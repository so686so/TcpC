/**
 * 파일명: include/CommonDef.h
 *
 * 개요:
 * 프로젝트 전반에서 사용되는 공통 데이터 구조체, 상수, 타입 정의.
 * 서버와 클라이언트가 모두 이 파일을 포함해야 한다.
 */

#ifndef COMMON_DEF_H
#define COMMON_DEF_H

#include <stdint.h>   // uint32_t, int32_t, uint8_t 등의 고정 폭 정수 타입 사용
#include <stdbool.h>  // bool, true, false 타입 사용
#include <string.h>   // NULL 매크로 및 memset 등의 메모리 함수 사용을 위해 포함

// --------------------------------------------------------------------------
// 1. 상수 정의
// --------------------------------------------------------------------------

#define TARGET_NAME_LEN  ( 7 + 1 ) // '\0' 제외한 길이 7을 강조하기 위해 작성
#define DEFAULT_BUF_SIZE 4096      // 버퍼의 기본 크기
#define CHECKSUM_LEN     1         // 체크섬의 크기 (1 byte)

// 보안 핸드셰이크용 타겟 코드
#define TARGET_SEC_STRATEGY "SEC_ARG"


// --------------------------------------------------------------------------
// 2-1. 에러 코드 정의 (Enum) : 패킷 파싱 및 처리 결과 상태 코드
// --------------------------------------------------------------------------

typedef enum
{
    PKT_SUCCESS = 0,         // 성공
    PKT_ERR_TOO_SHORT,       // 데이터가 최소 헤더 길이보다 짧음
    PKT_ERR_LENGTH_MISMATCH, // 헤더에 명시된 길이와 실제 수신 길이가 다름
    PKT_ERR_CHECKSUM_FAIL,   // 체크섬 검증 실패
    PKT_ERR_NULL_PTR         // 필수 인자가 NULL임
} PacketResult;


// --------------------------------------------------------------------------
// 2-2. 보안 전략 정의 (Enum)
// --------------------------------------------------------------------------

typedef enum
{
    SEC_STRATEGY_NONE = 0, // 평문 통신
    SEC_STRATEGY_XOR  = 1  // XOR 암호화
    // ... 차후 다른 보안 함수를 사용할 거면 여기에 추가하면 됨
} SecurityStrategy;


// --------------------------------------------------------------------------
// 3. 패킷 구조체 정의 (Wire Format)
//    실제 전송 데이터 = PacketHeader + Body(가변) + CheckSum(1 byte)
// --------------------------------------------------------------------------

/*
 * 현재 파일인 CommonDef.h 에서는 모든 서비스에서 공통으로 쓰일
 * 'PacketHeader' 구조체 및 'SecurityStrategyBody' 만 정의.
 *
 * Body에 해당하는 실제 구현부 구조체에 대한 예시를 확인하고 싶다면
 * examples/AppProtocol.h 헤더 내부의
 * 'LoginReqPacket', 'ChatPacket' 구조체를 확인.
 */

/** NOTE ( 매우 중요! )
 *
 *  Tcp 통신에서 구조체를 송신하기 위해서는 ( Raw Struct Transmission )
 *  1. 해당 구조체가 패딩 없이 1바이트 정렬이 되어야 하고,
 *  2. 구조체 내부에 주소 정보가 없이 값 정보만으로 이루어져야 한다.
 *
 *  만약 '#pragma pack(push, 1)' 명령으로 1바이트 정렬(패딩 제거)를 하지 않는다면
 *  패킷 송/수신 시 패킷에 의도하지 않은 패딩이 생겨 파싱이 이상하게 될 수 있다.
 *
 *  만약 구조체 내부에 포인터 값이 있다면,
 *  해당 포인터는 내 컴퓨터+프로그램에서만 유효한 '주소값'이기 때문에
 *  서버나 다른 클라이언트에서는 아예 무의미한 쓰레기값밖에 되지 않는다.
 */

// 네트워크로 전송되는 패킷의 '헤더' 부분 정의.
#pragma pack(push, 1)
typedef struct
{
    // 전체 패킷의 길이 (Header + Body + CheckSum)
    uint32_t total_len;

    // 패킷의 목적을 나타내는 문자열
    // 해당 문자열을 기준으로 Server/Client의 OnMessage 콜백에서
    // 분기 처리 가능
    // 예: "LOGIN", "CHAT"
    char target[TARGET_NAME_LEN];

} PacketHeader;
#pragma pack(pop)

// 최초 연결 시 암호화/복호화 전략 설정하는 구조체의 '바디' 부분 정의
#pragma pack(push, 1)
typedef struct
{
    // SecurityStrategy Enum 값
    int32_t strategy_code;

} SecurityStrategyBody;
#pragma pack(pop)


// --------------------------------------------------------------------------
// 4. 함수 포인터 타입 정의
//    암호화 및 복호화 로직을 교체(Strategy Pattern)할 수 있도록 정의.
// --------------------------------------------------------------------------

/**
 * ## 평문 데이터를 암호화한다. (제자리 변환 권장)
 *
 * ### [Params]
 * - data: 암호화할 데이터 포인터 (Header 이후의 Body 영역)
 * - len : 데이터 길이
 */
typedef void ( *EncryptFunc )( char* data, int len );

/**
 * ## 암호화된 데이터를 복호화한다.
 *
 * ### [Params]
 * - data: 복호화할 데이터 포인터
 * - len : 데이터 길이
 */
typedef void ( *DecryptFunc )( char* data, int len );


// --------------------------------------------------------------------------
// 4. 컨텍스트 전방 선언 (Forward Declaration)
// 설명: 상호 참조 문제를 방지하기 위해 포인터용으로 이름만 미리 선언.
// --------------------------------------------------------------------------

typedef struct TcpServerContext TcpServerContext;
typedef struct TcpClientContext TcpClientContext;

#endif // COMMON_DEF_H