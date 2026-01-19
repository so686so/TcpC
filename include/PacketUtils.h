/**
 * 파일명: include/PacketUtils.h
 *
 * 개요:
 * 패킷의 직렬화(생성), 역직렬화(파싱), 암호화, 체크섬 검증 함수 선언.
 * 네트워크 전송을 위한 바이트 스트림 변환을 담당한다.
 */

#ifndef PACKET_UTILS_H
#define PACKET_UTILS_H

#include "CommonDef.h"

// --------------------------------------------------------------------------
// 1. 암호화 / 복호화 유틸리티
// --------------------------------------------------------------------------

/**
 * ## 기본적인 XOR 연산을 수행한다. (암호화/복호화 로직이 동일함)
 *
 * ### [Param]
 * - data : 변환할 데이터 버퍼
 * - len  : 데이터 길이
 */
void Packet_DefaultXor( char* data, int len );


// --------------------------------------------------------------------------
// 2. 체크섬 유틸리티
// --------------------------------------------------------------------------

/**
 * ##   데이터의 무결성을 검증하기 위해 1바이트 체크섬을 계산한다.
 * #### 단순히 모든 바이트를 더한 값(Overflow 무시)을 반환한다.
 */
uint8_t Packet_CalcChecksum( const char* data, int len );


// --------------------------------------------------------------------------
// 3. 직렬화 (Serialize) - 송신용
// --------------------------------------------------------------------------

/**
 * ##   타겟 코드와 바디 데이터를 합쳐서 네트워크 전송용 패킷 버퍼를 생성한다.
 * #### 순서: 헤더(len, target) -> 바디 -> (바디 암호화) -> 체크섬 추가
 *
 * ### [Param]
 * - out_buffer   : 결과가 저장될 버퍼
 * - max_buf_size : 버퍼의 최대 크기 (오버플로우 방지)
 * - target_code  : 패킷 타겟 문자열 (예: "LOGIN")
 * - body_ptr     : 구조체 등 전송할 실제 데이터 포인터
 * - body_len     : 바디 데이터의 크기
 * - encrypt_func : 바디 암호화 함수 포인터 (NULL일 경우 암호화 안 함)
 *
 * ### [Return]
 * - 생성된 총 패킷 길이 (0보다 작으면 에러)
 */
int Packet_Serialize( char* out_buffer, int max_buf_size,
                      const char* target_code,
                      void* body_ptr, int body_len,
                      EncryptFunc encrypt_func );


// --------------------------------------------------------------------------
// 4. 역직렬화 (Deserialize) - 수신용
// --------------------------------------------------------------------------

/**
 * ##   수신된 버퍼에서 패킷을 파싱하고 검증한다.
 * #### 순서: 체크섬 검증 -> 길이 확인 -> 바디 복호화 -> 결과 반환
 * #### 주의: 복호화를 위해 in_buffer의 내용이 변경될 수 있음 (In-place Decryption)
 *
 * ### [Param]
 * - in_buffer    : 수신된 데이터 버퍼 (수정 가능해야 함)
 * - in_len       : 수신된 데이터 길이
 * - decrypt_func : 바디 복호화 함수 포인터 (NULL일 경우 복호화 안 함)
 * - out_target   : (출력) 추출된 타겟 문자열 버퍼 (최소 TARGET_NAME_LEN + 1 크기 필요)
 * - out_body_ptr : (출력) 바디 데이터가 시작되는 포인터 (in_buffer 내부 위치)
 * - out_body_len : (출력) 추출된 바디 데이터 길이
 *
 * ### [Return]
 * - PacketResult Enum (성공 시 PKT_SUCCESS)
 */
PacketResult Packet_Parse( char* in_buffer, int in_len,
                           DecryptFunc decrypt_func,
                           char* out_target,
                           char** out_body_ptr, int* out_body_len );

#endif // PACKET_UTILS_H