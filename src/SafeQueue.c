/**
 * 파일명: src/SafeQueue.c
 *
 * 개요:
 * SafeQueue.h 에 선언된 유한 버퍼 큐 구현부.
 */

#include "SafeQueue.h"

#include <stdio.h>   // printf, NULL
#include <stdlib.h>  // malloc, free (노드 및 큐 구조체 동적 할당/해제)
#include <pthread.h> // pthread_mutex_*, pthread_cond_* (동기화 객체 사용)

// --------------------------------------------------------------------------
// 내부 구조체 정의
// --------------------------------------------------------------------------

typedef struct Node
{
    void*        data;
    struct Node* next;
} Node;

struct SafeQueue
{
    Node* head; // Pop 위치
    Node* tail; // Push 위치

    int count;    // 현재 요소 개수
    int capacity; // 최대 허용 개수

    pthread_mutex_t mutex; // 동기화 객체
    pthread_cond_t  cond;  // 'Not Empty' 조건 변수 (소비자 대기용)
};


// --------------------------------------------------------------------------
// 함수 구현
// --------------------------------------------------------------------------

SafeQueue* SafeQueue_Create( int capacity )
{
    if( capacity <= 0 ){
        return NULL;
    }

    SafeQueue* queue = (SafeQueue*)malloc( sizeof( SafeQueue ) );
    if( !queue ){
        return NULL;
    }

    queue->head     = NULL;
    queue->tail     = NULL;
    queue->count    = 0;
    queue->capacity = capacity;

    if( pthread_mutex_init( &queue->mutex, NULL ) != 0 ){
        free( queue );
        return NULL;
    }

    if( pthread_cond_init( &queue->cond, NULL ) != 0 ){
        pthread_mutex_destroy( &queue->mutex );
        free( queue );
        return NULL;
    }

    return queue;
}

void SafeQueue_Destroy( SafeQueue* queue, FreeNodeFunc free_func )
{
    if( !queue )
        return;

    pthread_mutex_lock( &queue->mutex );

    Node* current = queue->head;

    while( current != NULL )
    {
        Node* next = current->next;

        // 사용자가 지정한 데이터 해제 함수 호출
        if( free_func && current->data ){
            free_func( current->data );
        }

        free( current ); // 노드 자체 해제
        current = next;
    }

    pthread_mutex_unlock( &queue->mutex );

    // 임계 영역 관리 관련 자원 해제
    pthread_mutex_destroy( &queue->mutex );
    pthread_cond_destroy ( &queue->cond  );

    free( queue );
}

bool SafeQueue_Enqueue( SafeQueue* queue, void* data )
{
    if( !queue || !data )
        return false;

    bool result = false;

    pthread_mutex_lock( &queue->mutex );
    {
        // 유한 큐 체크: 용량 초과 시 즉시 실패
        if( queue->count < queue->capacity )
        {
            Node* new_node = (Node*)malloc( sizeof( Node ) );

            // 메모리 할당 실패 시 방어
            if( new_node )
            {
                new_node->data = data;
                new_node->next = NULL;

                // 빈 큐에 원소를 삽입하는 경우
                if( queue->tail == NULL )
                {
                    // Head -> [ A ] <- Tail
                    // 신규 큐가 맨 앞인 동시에 맨 뒤
                    queue->head = new_node;
                    queue->tail = new_node;
                }
                // 큐에 이미 원소가 있는 경우
                else
                {
                    // Head -> [ A ] --------> [ N ] ^ Tail (아직 A를 가리킴)
                    // 기존 마지막 원소를 A라고 할 때,
                    // A 노드의 다음 원소를 새로운 원소라고 설정
                    queue->tail->next = new_node;

                    // Head -> [ A ] --------> [ N ] <- Tail (이제 B를 가리킴)
                    // 그리고 마지막 원소를 새로운 원소로 갱신
                    queue->tail = new_node;
                }

                queue->count++;
                result = true;

                // 대기 중인 소비자(Dequeue) 깨움
                pthread_cond_signal( &queue->cond );
            }
        }
    }
    pthread_mutex_unlock( &queue->mutex );

    return result;
}

void* SafeQueue_Dequeue( SafeQueue* queue )
{
    if( !queue ) return NULL;

    void* data = NULL;

    pthread_mutex_lock( &queue->mutex );
    {
        // 데이터가 없으면 대기 (Blocking)
        while( queue->count == 0 ){
            pthread_cond_wait( &queue->cond, &queue->mutex );
        }

        Node*  target_node = queue->head;
        data = target_node->data;

        queue->head = target_node->next;
        if( queue->head == NULL ){
            queue->tail = NULL;
        }

        queue->count--;

        free( target_node );
    }
    pthread_mutex_unlock( &queue->mutex );

    return data;
}

bool SafeQueue_IsEmpty( SafeQueue* queue )
{
    if( !queue )
        return true;

    bool is_empty = false;
    pthread_mutex_lock( &queue->mutex );
    {
        is_empty = ( queue->count == 0 );
    }
    pthread_mutex_unlock( &queue->mutex );

    return is_empty;
}

bool SafeQueue_IsFull( SafeQueue* queue )
{
    if( !queue )
        return false;

    bool is_full = false;
    pthread_mutex_lock( &queue->mutex );
    {
        is_full = ( queue->count >= queue->capacity );
    }
    pthread_mutex_unlock( &queue->mutex );

    return is_full;
}