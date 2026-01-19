/**
 * 파일명: include/CommonDef.h
 *
 * 개요:
 * 프로젝트 전반에서 사용되는 공통 데이터 구조체, 상수, 타입 정의.
 * 서버와 클라이언트가 모두 이 파일을 포함해야 한다.
 */

#ifndef COMMON_DEF_H
#define COMMON_DEF_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// --------------------------------------------------------------------------
// 1. 상수 정의
// --------------------------------------------------------------------------

#define TARGET_NAME_LEN  8    // 타겟 문자열(Target Code)의 고정 길이 (예: "LOGIN\0\0\0", "CHAT\0\0\0\0\0")
#define DEFAULT_BUF_SIZE 4096 // 버퍼의 기본 크기
#define CHECKSUM_LEN     1    // 체크섬의 크기 (1 byte)


// --------------------------------------------------------------------------
// 2. 에러 코드 정의 (Enum)
//    패킷 파싱 및 처리 결과 상태 코드
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
// 3. 패킷 구조체 정의 (Wire Format)
//    네트워크로 전송되는 패킷의 '헤더' 부분 정의.
//    실제 전송 데이터 = PacketHeader + Body(가변) + CheckSum(1 byte)
// --------------------------------------------------------------------------

// 1바이트 정렬 시작 (패딩 제거)
#pragma pack(push, 1)

typedef struct
{
    // 전체 패킷의 길이 (Header + Body + CheckSum)
    uint32_t total_len;

    // 패킷의 목적을 나타내는 문자열 (Null-terminated가 아닐 수 있음, 고정 길이)
    // 예: "LOGIN\0\0\0"
    char target[TARGET_NAME_LEN];

} PacketHeader;

// 정렬 설정 해제
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