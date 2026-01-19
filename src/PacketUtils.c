/**
 * 파일명: src/PacketUtils.c
 *
 * 개요:
 * PacketUtils.h 에 선언된 함수의 실제 구현부.
 */

#include "PacketUtils.h"

#include <stdio.h>
#include <arpa/inet.h>

// --------------------------------------------------------------------------
// 암호화 / 복호화 구현
// --------------------------------------------------------------------------

void Packet_DefaultXor( char* data, int len )
{
    const char key = 0x5A;
    for( int i = 0; i < len; ++i )
    {
        data[i] ^= key;
    }
}

// --------------------------------------------------------------------------
// 체크섬 구현
// --------------------------------------------------------------------------

uint8_t Packet_CalcChecksum( const char* data, int len )
{
    uint8_t sum = 0;
    for( int i = 0; i < len; ++i )
    {
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

    // 1. 길이 설정
    header->total_len = htonl( total_len );

    // 2. 타겟 설정 (NULL 문자로 초기화 후 복사)
    memset( header->target, 0, TARGET_NAME_LEN );
    if( target_code )
    {
        // 안전하게 최대 길이까지만 복사
        strncpy( header->target, target_code, TARGET_NAME_LEN );
    }

    // 3. 바디 복사
    char* body_pos = out_buffer + sizeof( PacketHeader );
    if( body_ptr && body_len > 0 )
    {
        memcpy( body_pos, body_ptr, body_len );

        // 4. 바디 암호화
        if( encrypt_func )
        {
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
    if( in_len < min_len )
    {
        return PKT_ERR_TOO_SHORT;
    }

    // 2. 헤더 길이 검증
    PacketHeader* header = (PacketHeader*)in_buffer;
    uint32_t total_len = ntohl( header->total_len );

    if( (int)total_len != in_len )
    {
        return PKT_ERR_LENGTH_MISMATCH;
    }

    // 3. 체크섬 검증
    char received_checksum = in_buffer[in_len - 1];
    uint8_t calc_checksum = Packet_CalcChecksum( in_buffer, in_len - 1 );

    if( (uint8_t)received_checksum != calc_checksum )
    {
        return PKT_ERR_CHECKSUM_FAIL;
    }

    // 4. 데이터 추출 및 반환값 설정

    // 4-1. 타겟 문자열 추출 (안전성 보장)
    if( out_target )
    {
        // 먼저 0으로 초기화
        memset( out_target, 0, TARGET_NAME_LEN + 1 );

        // 원본 데이터에서 최대 길이만큼 복사 (NULL 종료 보장을 위해 0으로 민 버퍼에 덮어씀)
        // 만약 header->target이 꽉 차 있어서 NULL이 없더라도,
        // 외부 버퍼(out_target)는 +1 크기를 가정하므로 마지막은 0으로 유지됨.
        memcpy( out_target, header->target, TARGET_NAME_LEN );

        // 확실하게 마지막 바이트 NULL 처리 (TARGET_NAME_LEN 크기만큼 할당받았다면 -1 인덱스에)
        // 하지만 보통 외부에서는 char buf[TARGET_NAME_LEN + 1]을 사용한다고 가정함.
        out_target[TARGET_NAME_LEN] = '\0';
    }

    // 4-2. 바디 정보 추출
    int body_len = total_len - sizeof( PacketHeader ) - CHECKSUM_LEN;
    char* body_pos = in_buffer + sizeof( PacketHeader );

    // 5. 바디 복호화 (In-place Decryption)
    // in_buffer는 char* (수정 가능)이므로 직접 복호화 수행
    if( decrypt_func && body_len > 0 )
    {
        decrypt_func( body_pos, body_len );
    }

    // 출력 변수 설정
    if( out_body_ptr ) *out_body_ptr = body_pos;
    if( out_body_len ) *out_body_len = body_len;

    return PKT_SUCCESS;
}