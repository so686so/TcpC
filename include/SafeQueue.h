/**
 * 파일명: include/SafeQueue.h
 *
 * 개요:
 * 스레드 간 안전한 데이터 전달을 위한 Bounded Blocking Queue 선언.
 * Mutex와 Condition Variable을 이용해 Producer-Consumer 패턴을 지원하며,
 * 최대 크기를 제한하여 메모리 과다 사용을 방지한다.
 */

#ifndef SAFE_QUEUE_H
#define SAFE_QUEUE_H

#include <stdbool.h> // bool

// --------------------------------------------------------------------------
// 1. 타입 정의
// --------------------------------------------------------------------------

typedef struct SafeQueue SafeQueue;

/**
 * ## 큐 내부의 데이터를 해제할 때 사용할 함수 포인터 타입
 *
 * 큐 내부의 개별 원소값인 data가 만약 구조체 형식이고
 * 자신 내부에 동적할당한 메모리값을 가지고 있다면
 * FreeNodeFunc 콜백 함수를 통해, 해당 동적 할당을 해제해 줘야 함.
 * 그렇지 않으면 data 자체만 해제되고, 그 내부에 있던 동적 할당된 메모리는
 * 해제되지 않은 채 dangling pointer가 되어 리소스 관리상 위험함.
 *
 * ### [Params]
 * - data: 삭제 대상 데이터 (void*)
 */
typedef void ( *FreeNodeFunc )( void* data );


// --------------------------------------------------------------------------
// 2. 함수 선언
// --------------------------------------------------------------------------

/**
 * ## 유한한 크기를 가지는 SafeQueue 객체를 생성한다.
 *
 * ### [Params]
 * - capacity : 큐가 담을 수 있는 최대 아이템 개수 (0보다 커야 함)
 *
 * ### [Return]
 * - 생성된 SafeQueue 포인터 (실패 시 NULL)
 */
SafeQueue* SafeQueue_Create( int capacity );

/**
 * ##   SafeQueue를 파괴하고 내부 메모리를 정리한다.
 * #### 큐에 남아있는 데이터는 free_func를 이용해 정리한다.
 *
 * ### [Param]
 * - queue     : 파괴할 큐 객체
 * - free_func : 데이터 해제용 콜백 (NULL일 경우 데이터는 해제하지 않고 노드만 삭제)
 */
void SafeQueue_Destroy( SafeQueue* queue, FreeNodeFunc free_func );

/**
 * ##   큐에 데이터를 추가한다. (Thread-Safe, Non-Blocking)
 * #### 큐가 가득 찼다면 대기하지 않고 즉시 실패를 반환한다.
 *
 * ### [Param]
 * - queue : 대상 큐 객체
 * - data  : 추가할 데이터 포인터 (NULL이 아니어야 함)
 *
 * ### [Return]
 * - true : 추가 성공
 * - false: 큐가 가득 차서 실패
 */
bool SafeQueue_Enqueue( SafeQueue* queue, void* data );

/**
 * ##   큐에서 데이터를 꺼낸다. (Blocking)
 * #### 큐가 비어있다면 데이터가 들어올 때까지 스레드를 대기시킨다.
 *
 * ### [Param]
 * - queue : 대상 큐 객체
 *
 * ### [Return]
 * - 꺼낸 데이터 포인터 (큐가 파괴되거나 오류 시 NULL)
 */
void* SafeQueue_Dequeue( SafeQueue* queue );

/**
 * ## 큐가 현재 비어있는지 확인한다. (Non-blocking)
 *
 * ### [Return]
 * - true  : 비어있음
 * - false : 데이터 있음
 */
bool SafeQueue_IsEmpty( SafeQueue* queue );

/**
 * ## 큐가 현재 가득 찼는지 확인한다. (Non-blocking)
 *
 * ### [Return]
 * - true  : 가득 참 (Enqueue 불가)
 * - false : 여유 있음
 */
bool SafeQueue_IsFull( SafeQueue* queue );

#endif // SAFE_QUEUE_H