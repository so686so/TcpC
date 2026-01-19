# TcpC 

### High-Performance Async TCP Framework

**TcpC**는 리눅스 환경(Epoll)에서 동작하는 **고성능 비동기 TCP 서버/클라이언트 프레임워크**입니다.
복잡한 네트워크 I/O 처리를 라이브러리 내부로 캡슐화하여, 개발자는 비즈니스 로직에만 집중할 수 있도록 설계되었습니다.

## 1. 주요 특징 및 장점 (Features)

* **고성능 비동기 I/O (High Performance)**
* Linux **Epoll (Edge Triggered)** 기반의 비동기 소켓 처리를 통해 대규모 동시 접속을 효율적으로 처리합니다.
* **Producer-Consumer 패턴**과 **Message Queue**를 적용하여 I/O 스레드와 워커 스레드를 분리, 병목 현상을 최소화했습니다.


* **스레드 안전성 & 데이터 무결성 (Thread Safety)**
* **Single Worker Thread 모델**을 채택하여, 사용자의 콜백 함수(비즈니스 로직) 내에서는 별도의 Lock 없이도 데이터 경쟁(Race Condition) 걱정 없이 안전하게 코딩할 수 있습니다. (Node.js, Redis와 유사한 이벤트 루프 방식)


* **강력한 클라이언트 기능**
* 별도의 스레드에서 네트워크를 관리하며, 연결이 끊어질 경우 **자동 재연결(Auto-Reconnection)** 을 수행합니다.
* `IsConnected()` 함수를 통해 직관적으로 연결 상태를 확인할 수 있습니다.


* **안정적인 프로토콜 & 보안**
* **Header + Body + Checksum** 구조의 패킷 시스템이 내장되어 있어 데이터 파편화(Fragmentation) 및 오염을 자동으로 처리합니다.
* 연결 수립 시 **보안 핸드셰이크(Handshake)** 과정을 통해 암호화 전략(현재 XOR 지원, 확장 가능)을 자동으로 협상합니다.


* **사용 편의성**
* 직관적인 콜백(Callback) 방식의 API (`OnMessage` 등)를 제공합니다.
* C11 표준을 준수하며 가볍고 의존성이 적습니다.



---

## 2. 예제 코드 빌드 및 실행 방법

이 프로젝트는 `examples/` 폴더 내에 채팅 서버(`MainServer.c`)와 클라이언트(`MainClient.c`) 예제를 포함하고 있습니다.

### 사전 준비

* Linux 환경 (Epoll 지원 OS)
* GCC 컴파일러
* CMake (선택 사항)

### 방법 A: CMake 사용 (권장)

```bash
# 1. 빌드 디렉터리 생성 및 이동
mkdir build
cd build

# 2. CMake 설정 및 빌드
cmake ..
make

# 3. 실행
# 서버 실행 (포트 3691)
./tcp_server 3691

# 클라이언트 실행 (새 터미널에서)
# 사용법: ./tcp_client <UserID> <ServerIP> <ServerPort>
# 예를 들어 서버의 IP가 192.168.1.100 일 경우
./tcp_client User1 192.168.1.100 3691

```

### 방법 B: GCC로 직접 컴파일

`include` 경로를 지정하고 `pthread` 라이브러리를 링크해야 합니다.

**서버 빌드 & 실행:**

```bash
# 컴파일
gcc -o server examples/MainServer.c src/*.c -I./include -lpthread

# 실행
./server 3691

```

**클라이언트 빌드 & 실행:**

```bash
# 컴파일
gcc -o client examples/MainClient.c src/*.c -I./include -lpthread

# 실행
# 예를 들어 서버의 IP가 192.168.1.100 일 경우
./client User1 192.168.1.100 3691

```

---

## 3. 라이브러리 가져다 쓰기 (Integration Guide)

TcpC 라이브러리를 본인의 프로젝트에 적용하려면 아래 파일들을 복사해가시면 됩니다.

### 3.1. 필요한 파일 및 폴더 구성

프로젝트의 구조를 아래와 같이 구성하는 것을 권장합니다.

```text
MyProject/
├── include/           <-- TcpC의 include 폴더 전체 복사
│   ├── CommonDef.h
│   ├── PacketUtils.h
│   ├── SafeQueue.h
│   ├── TcpClient.h
│   └── TcpServer.h
├── src/               <-- TcpC의 src 폴더 전체 복사
│   ├── PacketUtils.c
│   ├── SafeQueue.c
│   ├── TcpClient.c
│   └── TcpServer.c
└── main.c             <-- 본인의 소스 코드

```

### 3.2. 컴파일 방법

본인의 `main.c`를 컴파일할 때 `src/*.c`를 함께 컴파일하고, `include` 경로를 포함시켜주세요.

```bash
gcc -o my_app main.c src/*.c -I./include -lpthread

```

---

## 4. 최소 기능 예제 (Minimal Examples)

라이브러리를 사용하여 가장 단순하게 서버와 클라이언트를 구현하는 방법입니다.

### 4.1. 최소 서버 (SimpleServer.c)

받은 메시지를 콘솔에 출력하기만 하는 에코 서버입니다.

```c
#include <stdio.h>
#include <stdbool.h>
#include "TcpServer.h"

// 수신 콜백 함수
void OnMessage(TcpServerContext* ctx, int fd, void* arg, const char* target, const char* body, int len) {
    printf("[Recv] (From:%d) Target: %s, Body: %.*s\n", fd, target, len, body);
}

int main() {
    volatile bool exit = false;
    // 1. 서버 생성 (OnMessage의 void* arg 인자를 NULL로 설정)
    TcpServerContext* server = CreateTcpServerContext(OnMessage, NULL);

    // 2. 초기화 (포트 8080)
    if (server && server->Init(server, 8080)) {
        printf("Server Started on 8080\n");
        // 3. 서버 실행 (Blocking)
        server->Run(server, &exit);
    }

    server->Destroy(server);
    return 0;
}

```

### 4.2. 최소 클라이언트 (SimpleClient.c)

연결 후 "Hello" 메시지를 한 번 보내고 종료하는 클라이언트입니다.

```c
#include <stdio.h>
#include <unistd.h>
#include "TcpClient.h"

// 수신 콜백 (여기선 사용 안함)
void OnMessage(TcpClientContext* ctx, void* arg, const char* target, const char* body, int len) {}

int main() {
    // 1. 클라이언트 생성
    TcpClientContext* client = CreateTcpClientContext(OnMessage, NULL);

    // 2. 연결 시도 (비동기)
    client->Connect(client, "127.0.0.1", 8080);

    // 3. 연결 대기 (Handshake 완료 시점 체크)
    while (!client->IsConnected(client)) {
        usleep(10000); // 10ms 대기
    }

    // 4. 데이터 전송
    const char* msg = "Hello TcpC!";
    client->Send(client, "GREET", (void*)msg, 12); // GREET라는 타겟으로 전송

    printf("Message Sent!\n");

    sleep(1); // 전송 완료 대기
    client->Destroy(client);
    return 0;
}

```