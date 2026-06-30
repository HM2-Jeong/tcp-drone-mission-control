
//=======================================================================
//2024152029 정하밈 네트워크 프로그래밍 과제 - 드론 시뮬레이터 클라이언트
//=======================================================================

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <winsock2.h>
#include <process.h>
#include <math.h>
#include <time.h> // 랜덤 시드 초기화를 위한 헤더
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

// 서버와 동일한 구조체 패킹 설정
#pragma pack(push, 1)
typedef struct {
    int id;
    int status; // 0:대기, 1:1차이동, 2:1차도착, 3:2차이동, 4:완료, 5:종료
    double x, y, targetX, targetY;
} DronePacket;
#pragma pack(pop)

DronePacket myDrone;
HANDLE hMutex;
volatile int gDone = 0; // 쓰레드 간 종료 신호 공유 변수

unsigned WINAPI MoveThread(void* arg);
unsigned WINAPI CommThread(void* arg);


int main(int argc, char* argv[]) {

	if (argc != 2) { // 드론 ID 인자 체크 error handling
        printf("사용법: %s <드론ID(1~4)>\n", argv[0]);
        return 1;
    }

	// 1. 윈속 초기화
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

	myDrone.id = atoi(argv[1]); // ID를 string에서 int로 변환
    srand((unsigned)time(NULL) + myDrone.id);
    myDrone.x = -50.0 + (myDrone.id * 20.0);
    myDrone.y = 30.0;
    myDrone.status = 0;

	// 2. 서버 연결
    SOCKET hSock = socket(PF_INET, SOCK_STREAM, 0);
    SOCKADDR_IN adr;
    memset(&adr, 0, sizeof(adr));
    adr.sin_family = AF_INET;
    adr.sin_port = htons(9000);
    adr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(hSock, (SOCKADDR*)&adr, sizeof(adr)) == SOCKET_ERROR) {
        printf("[ID %d] 서버 연결 실패\n", myDrone.id);
        return 1;
    }
    printf("[ID %d] 기지국 연결 성공. 미션 대기 중...\n", myDrone.id);

    hMutex = CreateMutex(NULL, FALSE, NULL);

	HANDLE h[2]; // 0: 이동 쓰레드, 1: 통신 쓰레드
	h[0] = (HANDLE)_beginthreadex(NULL, 0, MoveThread, NULL, 0, NULL); // 물리적 움직임 담당 쓰레드
	h[1] = (HANDLE)_beginthreadex(NULL, 0, CommThread, &hSock, 0, NULL); // 서버와 통신 담당 쓰레드

    WaitForMultipleObjects(2, h, TRUE, INFINITE);
    CloseHandle(h[0]);
    CloseHandle(h[1]);

    printf("[ID %d] 3초 후 종료됩니다...\n", myDrone.id);
    Sleep(3000);

    closesocket(hSock);
    WSACleanup();
    CloseHandle(hMutex);
    return 0;
}


// 이동 로직 담당 쓰레드
unsigned WINAPI MoveThread(void* arg) {
	int tick = 0; // 시간 흐름 체크용 변수

	while (1) { // 종료 신호가 올 때까지 무한 루프
        WaitForSingleObject(hMutex, INFINITE);
        if (gDone) {
            ReleaseMutex(hMutex);
            break;
        }

        int s = myDrone.status;

        // 이동 중(1, 3)일 때 0.5초마다 현재 위치 보고
        if (s == 1 || s == 3) {
            tick++;
            if (tick % 5 == 0) {
                printf("[ID %d] 이동 중... 현재(%.1f, %.1f) -> 목표(%.1f, %.1f)\n",
                    myDrone.id, myDrone.x, myDrone.y, myDrone.targetX, myDrone.targetY);
            }
        }

        if (s == 0) {
            //랜덤 이동 폭 1.5
            myDrone.x += ((rand() % 31) - 15) * 0.1; // -1.5 ~ +1.5 범위
            myDrone.y += ((rand() % 31) - 15) * 0.1; // -1.5 ~ +1.5 범위

            // 경계 제한 (맵 밖으로 나가지 않게)
            if (myDrone.x < -90.0) myDrone.x = -90.0;
            if (myDrone.x > 90.0) myDrone.x = 90.0;
            if (myDrone.y < 20.0) myDrone.y = 20.0;
            if (myDrone.y > 180.0) myDrone.y = 180.0;
        }
        else if (s == 1 || s == 3) { // 1차 또는 2차 목표로 이동
            double dx = myDrone.targetX - myDrone.x;
            double dy = myDrone.targetY - myDrone.y;
			double d = sqrt(dx * dx + dy * dy); // 현재 위치와 목표 위치 사이의 거리 계산

            if (d > 0.5) {
                myDrone.x += (dx / d) * 0.5;
                myDrone.y += (dy / d) * 0.5;
            }
            else {
                // 목표 지점에 정확히 고정
                myDrone.x = myDrone.targetX;
                myDrone.y = myDrone.targetY;

                if (s == 1) {
                    myDrone.status = 2; // 1차 도착 완료 보고
                    printf("\n[ID %d] 1차 집결지 도착 완료!\n", myDrone.id);
                }
                else {
                    myDrone.status = 4; // 최종 도착 완료 보고
                    printf("\n[ID %d] 최종 목적지 도착! 서버 승인 대기 중...\n", myDrone.id);
                }
            }
        }
        else if (s == 5) { // 서버의 최종 종료 승인 수신 시
            printf("\n[ID %d] 기지국 종료 승인 확인. 안전하게 종료합니다.\n", myDrone.id);
            gDone = 1;
            ReleaseMutex(hMutex);
            break;
        }

        ReleaseMutex(hMutex);
        Sleep(100);
    }
    return 0;
}

// 통신 담당 쓰레드
unsigned WINAPI CommThread(void* arg) {
	SOCKET hSock = *((SOCKET*)arg); // 메인함수에서 전달된 소켓 받아옴
	DronePacket recvP = { 0 }; // 서버로부터 수신한 패킷 저장용 구조체

    while (1) {
        WaitForSingleObject(hMutex, INFINITE);
        DronePacket sendP = myDrone;
        ReleaseMutex(hMutex);

        // 송신 루프
        int sent = 0;
        while (sent < (int)sizeof(DronePacket)) {
            int len = send(hSock, ((char*)&sendP) + sent, sizeof(DronePacket) - sent, 0);
            if (len <= 0) goto COMM_END;
            sent += len;
        }

        // 수신 루프
        int rcvd = 0;
        while (rcvd < (int)sizeof(DronePacket)) {
            int len = recv(hSock, ((char*)&recvP) + rcvd, sizeof(DronePacket) - rcvd, 0);
            if (len <= 0) goto COMM_END;
            rcvd += len;
        }

        WaitForSingleObject(hMutex, INFINITE);
        // 서버가 내린 새로운 상태 명령만 적용
        if (recvP.status > myDrone.status) {
            if (recvP.status == 1) printf("\n[ID %d] 1차 미션 시작!\n", myDrone.id);
            if (recvP.status == 3) printf("\n[ID %d] 2차 미션 시작! (목표 X: %.1f)\n", myDrone.id, recvP.targetX);

            myDrone.status = recvP.status;
            myDrone.targetX = recvP.targetX;
            myDrone.targetY = recvP.targetY;
        }
        ReleaseMutex(hMutex);

        if (gDone) break;
        Sleep(200);
    }

COMM_END:
    WaitForSingleObject(hMutex, INFINITE);
    gDone = 1;
    ReleaseMutex(hMutex);
    return 0;
}

