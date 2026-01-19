/**
 * 파일명: examples/AppProtocol.h
 *
 * 개요:
 * 서버와 클라이언트가 공유하는 비즈니스 데이터 구조체.
 */

#ifndef APP_PROTOCOL_H
#define APP_PROTOCOL_H

#include <stdint.h>

#pragma pack(push, 1)

// --------------------------------------------------------------------------
// 1. 로그인 요청 (Client -> Server)
// 타겟 코드: "LOGIN"
// --------------------------------------------------------------------------
typedef struct
{
    char user_id[32];
    char password[32];
    int version;
} LoginReqPacket;

// --------------------------------------------------------------------------
// 2. 채팅 메시지 (Client <-> Server)
// 타겟 코드: "CHAT"
// --------------------------------------------------------------------------
typedef struct
{
    char sender_id[32];
    char message[128];
    uint64_t timestamp;
} ChatPacket;

#pragma pack(pop)

#endif // APP_PROTOCOL_H