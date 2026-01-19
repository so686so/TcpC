/**
 * 파일명: src/PacketUtils.c
 *
 * 개요:
 * PacketUtils.h 에 선언된 함수의 실제 구현부.
 */

#include "PacketUtils.h"

#include <stdio.h>     // printf (디버깅용), NULL 매크로
#include <arpa/inet.h> // htonl, ntohl (네트워크 바이트 오더 변환)

// --------------------------------------------------------------------------
// 암호화 / 복호화 구현
// --------------------------------------------------------------------------

void Packet_DefaultXor( char* data, int len )
{
    // 해당 값은 수정 가능, 혹은 외부에서 인자로 받아올 수 있음.
    const char key = 0x5A;

    for( int i = 0; i < len; ++i ){
        data[i] ^= key;
    }
}

// --------------------------------------------------------------------------
// 체크섬 구현
// --------------------------------------------------------------------------

uint8_t Packet_CalcChecksum( const char* data, int len )
{
    uint8_t sum = 0;
    for( int i = 0; i < len; ++i ){
        sum += (uint8_t)data[i];
    }
    return sum;
}

// --------------------------------------------------------------------------
// 직렬화 (Serialize) 구현
// --------------------------------------------------------------------------

int Packet_Serialize( char* out_buffer, int max_buf_size,
                      const char* target_code,
                      void* body_ptr, int body_len,
                      EncryptFunc encrypt_func )
{
    int total_len = sizeof( PacketHeader ) + body_len + CHECKSUM_LEN;

    if( total_len > max_buf_size )
    {
        return -1;
    }

    PacketHeader* header = (PacketHeader*)out_buffer;

    // 1. Header->len 설정
    header->total_len = htonl( total_len );

    // 2. Header->Target 설정
    memset( header->target, 0, TARGET_NAME_LEN );
    if( target_code ){
        strncpy( header->target, target_code, TARGET_NAME_LEN );
    }

    // 3. Body 복사
    char* body_pos = out_buffer + sizeof( PacketHeader ); // 복사할 위치 찾기

    if( body_ptr && body_len > 0 ){
        memcpy( body_pos, body_ptr, body_len );

        // 4. Body 암호화
        if( encrypt_func ){
            encrypt_func( body_pos, body_len );
        }
    }

    // 5. 체크섬 계산
    // (헤더 + 암호화된 바디) 까지 계산
    int calc_range = sizeof( PacketHeader ) + body_len;
    uint8_t checksum = Packet_CalcChecksum( out_buffer, calc_range );

    out_buffer[calc_range] = (char)checksum;

    return total_len;
}

// --------------------------------------------------------------------------
// 역직렬화 (Deserialize) 구현
// --------------------------------------------------------------------------

PacketResult Packet_Parse( char* in_buffer, int in_len,
                           DecryptFunc decrypt_func,
                           char* out_target,
                           char** out_body_ptr, int* out_body_len )
{
    // 1. 최소 길이 검사
    int min_len = sizeof( PacketHeader ) + CHECKSUM_LEN;
    if( in_len < min_len ){
        return PKT_ERR_TOO_SHORT;
    }

    // 2. 헤더 길이 검증
    PacketHeader* header = (PacketHeader*)in_buffer;
    uint32_t total_len = ntohl( header->total_len );

    if( (int)total_len != in_len ){
        return PKT_ERR_LENGTH_MISMATCH;
    }

    // 3. 체크섬 검증
    char    recv_checksum = in_buffer[in_len - 1];
    uint8_t calc_checksum = Packet_CalcChecksum( in_buffer, in_len - 1 );

    if( (uint8_t)recv_checksum != calc_checksum ){
        return PKT_ERR_CHECKSUM_FAIL;
    }

    // 4. 데이터 추출 및 반환값 설정

    // 4-1. 타겟 문자열 추출 (안전성 보장)
    if( out_target )
    {
        // 먼저 0으로 초기화 후 값 복사
        memset( out_target, 0, TARGET_NAME_LEN );
        memcpy( out_target, header->target, TARGET_NAME_LEN );

        // 확실하게 마지막 바이트 NULL 처리
        out_target[TARGET_NAME_LEN - 1] = '\0';
    }

    // 4-2. 바디 정보 추출
    int   body_len = total_len - sizeof( PacketHeader ) - CHECKSUM_LEN;
    char* body_pos = in_buffer + sizeof( PacketHeader );

    // 5. 바디 복호화 (In-place Decryption)
    // in_buffer는 char* (수정 가능)이므로 직접 복호화 수행
    if( decrypt_func && body_len > 0 ){
        decrypt_func( body_pos, body_len );
    }

    // 출력 변수 설정
    if( out_body_ptr ) *out_body_ptr = body_pos;
    if( out_body_len ) *out_body_len = body_len;

    return PKT_SUCCESS;
}

EncryptFunc Packet_GetEncryptFunc( int strategy_code )
{
    switch( strategy_code )
    {
    case SEC_STRATEGY_XOR:
        return Packet_DefaultXor;

    case SEC_STRATEGY_NONE:
    default:
        return NULL; // 암호화 없음
    }
}

DecryptFunc Packet_GetDecryptFunc( int strategy_code )
{
    switch( strategy_code )
    {
    case SEC_STRATEGY_XOR:
        return Packet_DefaultXor;

    case SEC_STRATEGY_NONE:
    default:
        return NULL; // 복호화 없음
    }
}