//=========================================================================
//2024152029 정하밈 네트워크 프로그래밍 과제 - 드론 제어 시뮬레이터 (서버 측)
//=========================================================================

#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <process.h> // 멀티 스레드 관련
#include <math.h>
#include <windows.h> // API 및 콘솔 제어 관련
#include <conio.h> // 콘솔 입출력 제어
#pragma comment(lib, "Ws2_32.lib")

#define MAX_DRONES 4
#define MIN_DISTANCE 10.0

// --- UI 및 좌표 설정 
// MAP_X, MAP_Y: 맵의 왼쪽 상단 좌표
#define MAP_X 5
#define MAP_Y 3

// MAP_W, MAP_H: 맵의 너비와 높이 (콘솔 문자 단위)
#define MAP_W 60
#define MAP_H 20

// WORLD_MIN_X, WORLD_MAX_X, WORLD_MIN_Y, WORLD_MAX_Y: 실제 세계 좌표 범위 (미터 단위)
#define WORLD_MIN_X -100.0
#define WORLD_MAX_X  100.0
#define WORLD_MIN_Y   0.0
#define WORLD_MAX_Y  200.0

#pragma pack(push, 1) // 패킷 구조체를 1바이트 단위로 정렬하여 크기를 최소화
typedef struct {
    int id;
	int status; // 0:대기, 1:1차이동, 2:1차도착, 3:2차이동, 4:2차도착, 5:종료 명령
    double x, y;
    double targetX, targetY;
} DronePacket;
#pragma pack(pop)

DronePacket drone_list[MAX_DRONES];
HANDLE hMutex; // 한 드론 정보 수정 시 UI 스레드와 충돌 방지 

// --- 전역 플래그 변수
int isMissionStarted = 0; // 1차 미션 엔터 대기 플래그
int isSecondMissionReady = 0; // 2차 미션 엔터 대기 플래그

// 드론 위치 업데이트 시 이전 위치를 지우기 위한 배열
int prevX[MAX_DRONES] = { 0 };
int prevY[MAX_DRONES] = { 0 };

void gotoXY(int x, int y) { // 콘솔 커서 이동 함수
    COORD pos = { (short)x, (short)y };
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), pos);
}

// 실제 세계 좌표를 콘솔 맵 좌표로 변환하는 함수
int toX(double wx) { return MAP_X + (int)((wx - WORLD_MIN_X) / (WORLD_MAX_X - WORLD_MIN_X) * MAP_W); }
int toY(double wy) { return MAP_Y + MAP_H - (int)((wy - WORLD_MIN_Y) / (WORLD_MAX_Y - WORLD_MIN_Y) * MAP_H); }
double calcDist(double x1, double y1, double x2, double y2) { return sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1)); }

// --- 함수 선언
void drawServerUI();
unsigned WINAPI HandleDrone(LPVOID arg);
unsigned WINAPI UIThread(void* arg);
unsigned WINAPI ControlThread(void* arg);

int main(int argc, char* argv[]) {
	// 1. 윈속 초기화 및 서버 소켓 설정
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);

	// 2. 뮤텍스 생성 및 드론 리스트 초기화
    hMutex = CreateMutex(NULL, FALSE, NULL);

	// 3. 서버 소켓 생성, 바인드, 리슨
    memset(drone_list, 0, sizeof(drone_list));
    SOCKET hServ = socket(PF_INET, SOCK_STREAM, 0);
    SOCKADDR_IN adr = { AF_INET, htons(9000), htonl(INADDR_ANY) };
    bind(hServ, (SOCKADDR*)&adr, sizeof(adr));
    listen(hServ, 5);

	// 4. UI 스레드 및 컨트롤 스레드 시작
    _beginthreadex(NULL, 0, UIThread, NULL, 0, NULL);
    _beginthreadex(NULL, 0, ControlThread, NULL, 0, NULL);

	// 5. 클라이언트(드론) 연결 대기 및 처리
    while (1) {
        SOCKADDR_IN cAdr; int sz = sizeof(cAdr);
        SOCKET hClnt = accept(hServ, (SOCKADDR*)&cAdr, &sz);
        if (hClnt == INVALID_SOCKET) continue;
        SOCKET* pS = (SOCKET*)malloc(sizeof(SOCKET)); *pS = hClnt; 
		_beginthreadex(NULL, 0, HandleDrone, pS, 0, NULL); // 각 드론 연결마다 별도의 스레드로 처리
    }
    return 0;
}


void drawServerUI() {
    //WaitForSingleObject(hMutex, INFINITE);
    gotoXY(0, 0);
    int stage1Finished = 0;
    int stage2Finished = 0;
	int connectedCount = 0; // 접속된 드론 수 카운트

    for (int i = 0; i < MAX_DRONES; i++) {
        if (drone_list[i].id != 0) {
            connectedCount++;
            if (drone_list[i].status >= 2) stage1Finished++;
            if (drone_list[i].status == 4) stage2Finished++;
        }
    }

    // --- 상단 메시지 출력부 ---
    if (!isMissionStarted) {
        printf("    ========= [기지국] ENTER -> 1차 미션 시작 =========                  ");
    }
    else if (!isSecondMissionReady) {
        if (connectedCount > 0 && stage1Finished == connectedCount) {
            // 1차 완료 시 간격 검증 로직
            int safe = 1;
            double min_dist = 999.0;

            for (int i = 0; i < MAX_DRONES; i++) {
                for (int j = i + 1; j < MAX_DRONES; j++) {
                    if (drone_list[i].id != 0 && drone_list[j].id != 0) {
                        double d = calcDist(drone_list[i].x, drone_list[i].y, drone_list[j].x, drone_list[j].y);
                        if (d < min_dist) min_dist = d;
                        if (d < MIN_DISTANCE) safe = 0;
                    }
                }
            }

            if (safe)
                printf("   ========= [1차 완료] 모든 간격 10m 이상 확인! (최소:%.1fm) =========  ", min_dist);
            else
                printf("   ========= [주의] 일부 드론 간격 10m 미만! (최소:%.1fm) =========      ", min_dist);

            gotoXY(0, 1);
            printf("                   (ENTER를 누르면 2차 미션 시작)                     ");
        }
        else {
            printf("    ========= [기지국] 1차 미션 수행 중... =========                    ");
        }
    }
    else {
        if (connectedCount > 0 && stage2Finished == connectedCount)
            printf(    "========= [최종 완료] 이동 완료 및 모든 드론 10m 간격 유지 중 ========= ");
        else
            printf("    ========= [기지국] 2차 미션 수행 중... =========                    ");
    }

    gotoXY(toX(0), toY(100)); printf("+");

	for (int i = 0; i < MAX_DRONES; i++) { // 드론 위치 업데이트
        if (drone_list[i].id != 0) {
            if (prevX[i] != 0) { gotoXY(prevX[i], prevY[i]); printf(" "); }
            int cx = toX(drone_list[i].x);
            int cy = toY(drone_list[i].y);
            gotoXY(cx, cy); printf("%d", drone_list[i].id);
            prevX[i] = cx; prevY[i] = cy;
        }
    }

    // 하단 정보 (Offline 메시지 고정)
    for (int i = 0; i < MAX_DRONES; i++) {
        gotoXY(0, MAP_Y + MAP_H + 3 + i);
        if (drone_list[i].id != 0) {
            printf("[ID %d] Stat:%d Pos:(%6.1f,%6.1f) Target:(%6.1f,%6.1f)    ",
                drone_list[i].id, drone_list[i].status, drone_list[i].x, drone_list[i].y, drone_list[i].targetX, drone_list[i].targetY);
        }
        else {
            printf("[ID %d] Offline...                                               ", i + 1);
        }
    }
}

unsigned WINAPI HandleDrone(LPVOID arg) { // 각 드론 연결 처리 스레드
    SOCKET hClntSock = *((SOCKET*)arg); 
    free(arg); 
    DronePacket p = { 0 }; 

    while (1) {
        int rcvd = 0;
        while (rcvd < (int)sizeof(DronePacket)) {
			int len = recv(hClntSock, ((char*)&p) + rcvd, sizeof(DronePacket) - rcvd, 0); // 패킷이 완전히 수신될 때까지 반복
			if (len <= 0) goto DISCONNECT; // 연결 종료 또는 오류 시 루프 탈출
            rcvd += len;
        }

        WaitForSingleObject(hMutex, INFINITE);
        if (p.id >= 1 && p.id <= MAX_DRONES) {
            // [1단계] 1차 미션 시작 (엔터 입력 시)
            if (isMissionStarted && p.status == 0) {
                if (p.id == 1) { p.targetX = 0;   p.targetY = 125; }
                else if (p.id == 2) { p.targetX = -15; p.targetY = 100; }
                else if (p.id == 3) { p.targetX = 15;  p.targetY = 100; }
                else if (p.id == 4) { p.targetX = 0;   p.targetY = 85; }
                p.status = 1; // 1차 이동 시작
            }
            // [2단계] 1차 도착 판정 (거리가 0.5m 이내면 도착 완료 처리)
            else if (p.status == 1 && calcDist(p.x, p.y, p.targetX, p.targetY) <= 0.05) {
                p.status = 2; // 1차 도착 완료 보고용
            }
            // [3단계] 2차 미션 시작 (2차 엔터 입력 시)
            else if (isSecondMissionReady && p.status == 2) {
                // 현재 위치에서 왼쪽으로 50m 이동하는 목표 설정
                p.targetX = p.x - 50.0;
                p.targetY = p.y;
                p.status = 3; // 2차 이동 시작
            }
            // [4단계] 2차 도착 판정
            else if (p.status == 3 && calcDist(p.x, p.y, p.targetX, p.targetY) <= 0.05) {
                p.status = 4; // 최종 완료
            }

            drone_list[p.id - 1] = p; // 서버 리스트 갱신

            // [5단계] 모두 완료 시 종료 명령(5)
            int connected = 0, finished = 0;
            for (int i = 0; i < MAX_DRONES; i++) {
                if (drone_list[i].id != 0) {
                    connected++;
                    if (drone_list[i].status >= 4) finished++;
                }
            }
            if (connected > 0 && connected == finished) {
                p.status = 5;
                drone_list[p.id - 1].status = 5;
            }
        }
        ReleaseMutex(hMutex);
        send(hClntSock, (char*)&p, sizeof(DronePacket), 0);
    }
DISCONNECT:
    closesocket(hClntSock);
    return 0;
}

unsigned WINAPI UIThread(void* arg) {
    system("cls");
    gotoXY(MAP_X - 1, MAP_Y - 1);
    for (int i = 0; i <= MAP_W + 1; i++) printf("-");
    for (int i = 0; i <= MAP_H; i++) {
        gotoXY(MAP_X - 1, MAP_Y + i); printf("|");
        gotoXY(MAP_X + MAP_W + 1, MAP_Y + i); printf("|");
    }
    gotoXY(MAP_X - 1, MAP_Y + MAP_H + 1);
    for (int i = 0; i <= MAP_W + 1; i++) printf("-");
    gotoXY(0, MAP_Y); printf("200m"); gotoXY(0, MAP_Y + MAP_H / 2); printf("100m"); gotoXY(0, MAP_Y + MAP_H); printf("  0m");

	CONSOLE_CURSOR_INFO ci = { 100, FALSE }; // 커서 크기 100, 보이지 않음
	SetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci); // 커서 깜빡임 제거
    while (1) {
		WaitForSingleObject(hMutex, INFINITE); // UI 업데이트 시 드론 정보와 충돌 방지
		drawServerUI(); // UI 업데이트
		ReleaseMutex(hMutex); // 뮤텍스 해제
        Sleep(100);
    }
    return 0;
}

unsigned WINAPI ControlThread(void* arg) {
    // 1. 첫 번째 엔터 대기 (입력 찌꺼기 완벽 제거)
    while (_kbhit()) _getch(); // 버퍼 비우기

    while (1) {
        if (_kbhit()) {
            if (_getch() == 13) break; // 13은 엔터키
        }
        Sleep(50);
    }

	WaitForSingleObject(hMutex, INFINITE); // 1차 미션 시작 플래그 설정
    isMissionStarted = 1;
	ReleaseMutex(hMutex); // 1차 미션 시작 신호

    // 2. 모든 드론이 1차 목표에 도달할 때까지 대기
    while (1) {
        int ready = 0;
        int connected = 0; // 접속된 드론 수 파악
        WaitForSingleObject(hMutex, INFINITE);
        for (int i = 0; i < MAX_DRONES; i++) {
            if (drone_list[i].id != 0) {
                connected++;
                if (drone_list[i].status >= 2) ready++;
            }
        }
        ReleaseMutex(hMutex);

        // 접속된 드론이 1대라도 있고, 그 드론들이 모두 준비되면 통과
        if (connected > 0 && ready == connected) break;
        Sleep(100);
    }

    // 3. 두 번째 엔터 대기 전 따닥(더블 클릭) 방지를 위해 찌꺼기 비우기
    while (_kbhit()) _getch();

    while (1) {
        if (_kbhit()) {
            if (_getch() == 13) break;
        }
        Sleep(50);
    }

    WaitForSingleObject(hMutex, INFINITE);
    isSecondMissionReady = 1;
    ReleaseMutex(hMutex);

    return 0;
}

