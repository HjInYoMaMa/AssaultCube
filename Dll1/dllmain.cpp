#include <windows.h>
#include <gl/GL.h>
#include <stdio.h>
#include <stdarg.h>
#include <vector>
#include <cmath>
#pragma comment(lib, "opengl32.lib")

// ═══════════════════════════════════════════════════════
//  OFFSETS
// ═══════════════════════════════════════════════════════
#define MAX_PLAYERS 32
#define ENTITY_LIST_OFFSET     0x10F4F8
#define PLAYER_COUNT_OFFSET    0x10F500
#define VIEW_MATRIX_OFFSET     0x101AE8

// Entity offsets
#define playerBaseOffset       0x10F4F4
#define entListBaseOffset      0x10F4F8
#define playerCount            0x10F500
#define gameStateOffset        0x10F49C
#define viewMatrixOffset       0x101AE8
#define weaponRecoilOffset     0x63786
#define crosshairEntOffset     0x607C0
#define playerHealthOffset     0xF8
#define playerArmorOffset      0x158
#define playerAmmoOffset       0x150
#define playerNameOffset       0x225
#define playerTeamOffset       0x32C
#define playerShootingOffset   0x224
#define rotationOffset         0x40
#define posOffset              0x4

// ═══════════════════════════════════════════════════════
//  STRUCTS
// ═══════════════════════════════════════════════════════
struct Vector3 { float x, y, z; };
struct Vector4 { float x, y, z, w; };
struct Vector2 { float x, y; };

struct Player {
    char unknown1[4];
    float x, y, z;
    char unknown2[0x30];
    float yaw;
    float pitch;
    char unknown3[0x1DD];
    char name[16];
};

// ═══════════════════════════════════════════════════════
//  MEM NAMESPACE
// ═══════════════════════════════════════════════════════
namespace mem {
    void Patch(BYTE* dst, BYTE* src, unsigned int size) {
        DWORD oldprotect;
        VirtualProtect(dst, size, PAGE_EXECUTE_READWRITE, &oldprotect);
        memcpy(dst, src, size);
        VirtualProtect(dst, size, oldprotect, &oldprotect);
    }

    void Nop(BYTE* dst, unsigned int size) {
        DWORD oldprotect;
        VirtualProtect(dst, size, PAGE_EXECUTE_READWRITE, &oldprotect);
        memset(dst, 0x90, size);
        VirtualProtect(dst, size, oldprotect, &oldprotect);
    }

    uintptr_t FindDMAAddy(uintptr_t ptr, std::vector<unsigned int> offsets) {
        uintptr_t addr = ptr;
        for (unsigned int i = 0; i < offsets.size(); ++i) {
            addr = *(uintptr_t*)addr;
            addr += offsets[i];
        }
        return addr;
    }
}

// ═══════════════════════════════════════════════════════
//  SAFE READ/WRITE
// ═══════════════════════════════════════════════════════
template<typename T>
T SafeRead(uintptr_t addr) {
    __try { return *(T*)addr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return T{}; }
}

template<typename T>
void SafeWrite(uintptr_t addr, T val) {
    __try { *(T*)addr = val; }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ═══════════════════════════════════════════════════════
//  GLOBALS
// ═══════════════════════════════════════════════════════
typedef BOOL(__stdcall* twglSwapBuffers)(HDC);
twglSwapBuffers oSwapBuffers = nullptr;

HMODULE g_hMod = nullptr;
bool    g_Menu = false;
int     g_Sel = 0;
uintptr_t moduleBase = 0;

bool bGodMode = false;
bool bInfAmmo = false;
bool bInfGrenades = false;
bool bInfArmour = false;
bool bNoRecoil = false;
bool bSpeedHack = false;
bool bESP = false;
bool bAimbot = false;
bool bFOV = false;

float aimbotFOV = 150.0f;
uintptr_t lastTarget = 0;

struct MenuItem { const char* label; bool* val; };
MenuItem g_Items[] = {
    { "God Mode",       &bGodMode     },
    { "Infinite Ammo",  &bInfAmmo     },
    { "Infinite Grenades", &bInfGrenades },
    { "Infinite Armour", &bInfArmour  },
    { "No Recoil",      &bNoRecoil    },
    { "Speed Hack",     &bSpeedHack   },
    { "ESP",            &bESP         },
    { "Aimbot",         &bAimbot      },
    { "FOV Circle",     &bFOV         },
};
const int g_Count = sizeof(g_Items) / sizeof(g_Items[0]);

static BYTE recoilOrig[10] = { 0x50,0x8D,0x4C,0x24,0x1C,0x51,0x8B,0xCE,0xFF,0xD2 };
static bool recoilNoped = false;

// ═══════════════════════════════════════════════════════
//  GAME ADDRESSES
// ═══════════════════════════════════════════════════════
uintptr_t GetBase() {
    return (uintptr_t)GetModuleHandle(NULL);
}

__declspec(noinline) uintptr_t GetPlayerPtrSafe() {
    uintptr_t base = GetBase();
    if (!base) return 0;
    uintptr_t ptr = *(uintptr_t*)(base + playerBaseOffset);
    if (ptr < 0x10000) return 0;
    return ptr;
}

// ═══════════════════════════════════════════════════════
//  WORLD TO SCREEN
// ═══════════════════════════════════════════════════════
bool WorldToScreen(const Vector3& pos, Vector2& screen, float matrix[16], int windowWidth, int windowHeight) {
    float x = pos.x * matrix[0] + pos.y * matrix[4] + pos.z * matrix[8] + matrix[12];
    float y = pos.x * matrix[1] + pos.y * matrix[5] + pos.z * matrix[9] + matrix[13];
    float z = pos.x * matrix[2] + pos.y * matrix[6] + pos.z * matrix[10] + matrix[14];
    float w = pos.x * matrix[3] + pos.y * matrix[7] + pos.z * matrix[11] + matrix[15];

    if (w < 0.01f) return false;

    float mX = (float)windowWidth / 2.0f;
    float mY = (float)windowHeight / 2.0f;

    screen.x = mX + (mX * x / w);
    screen.y = mY - (mY * y / w);

    return true;
}

// ═══════════════════════════════════════════════════════
//  OPENGL DRAWING FUNCTIONS
// ═══════════════════════════════════════════════════════
void GetVP(int& w, int& h) {
    GLint v[4]; glGetIntegerv(GL_VIEWPORT, v); w = v[2]; h = v[3];
}

void Begin2D(int w, int h) {
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, w, h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_LIGHTING);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void End2D() {
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW);  glPopMatrix();
    glPopAttrib();
}

void FillRect(float x, float y, float w, float h,
    float r, float g, float b, float a) {
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

void Border(float x, float y, float w, float h, float t,
    float r, float g, float b, float a) {
    FillRect(x, y, w, t, r, g, b, a);
    FillRect(x, y + h - t, w, t, r, g, b, a);
    FillRect(x, y, t, h, r, g, b, a);
    FillRect(x + w - t, y, t, h, r, g, b, a);
}

void DrawRect(float x, float y, float w, float h,
    float r, float g, float b, float a, float thickness = 1.0f) {
    glColor4f(r, g, b, a);
    glLineWidth(thickness);
    glBegin(GL_LINE_LOOP);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

void DrawFilledRect(float x, float y, float w, float h,
    float r, float g, float b, float a) {
    glColor4f(r, g, b, a);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

void DrawLine(float x1, float y1, float x2, float y2,
    float r, float g, float b, float a, float thickness = 1.0f) {
    glColor4f(r, g, b, a);
    glLineWidth(thickness);
    glBegin(GL_LINES);
    glVertex2f(x1, y1);
    glVertex2f(x2, y2);
    glEnd();
}

void DrawCircle(float cx, float cy, float radius, float r, float g, float b, float a, int segments = 32) {
    glColor4f(r, g, b, a);
    glBegin(GL_LINE_LOOP);
    for (int i = 0; i < segments; i++) {
        float theta = 2.0f * 3.14159265f * (float)i / (float)segments;
        float x = cx + radius * cosf(theta);
        float y = cy + radius * sinf(theta);
        glVertex2f(x, y);
    }
    glEnd();
}

GLuint g_FontBase = 0;
void InitFont(HDC hdc) {
    if (g_FontBase) return;
    g_FontBase = glGenLists(96);
    HFONT font = CreateFontA(
        13, 0, 0, 0, FW_BOLD, 0, 0, 0,
        ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FF_DONTCARE | DEFAULT_PITCH, "Tahoma");
    HFONT old = (HFONT)SelectObject(hdc, font);
    wglUseFontBitmaps(hdc, 32, 96, g_FontBase);
    SelectObject(hdc, old);
    DeleteObject(font);
}

void DrawStr(float x, float y, float r, float g, float b, float a,
    const char* fmt, ...) {
    char buf[256];
    va_list args; va_start(args, fmt);
    vsprintf_s(buf, fmt, args); va_end(args);
    glColor4f(r, g, b, a);
    glRasterPos2f(x, y + 12);
    glPushAttrib(GL_LIST_BIT);
    glListBase(g_FontBase - 32);
    glCallLists((GLsizei)strlen(buf), GL_UNSIGNED_BYTE, buf);
    glPopAttrib();
}

// ═══════════════════════════════════════════════════════
//  ESP DRAWING
// ═══════════════════════════════════════════════════════
void DrawESP() {
    if (!bESP) return;

    uintptr_t base = GetBase();
    if (!base) return;

    uintptr_t localPlayer = GetPlayerPtrSafe();
    if (!localPlayer) return;

    __try {
        Player* player = (Player*)localPlayer;

        int currentPlayers = *(int*)(base + PLAYER_COUNT_OFFSET);
        if (currentPlayers <= 0 || currentPlayers > MAX_PLAYERS) return;

        uintptr_t entityList = *(uintptr_t*)(base + ENTITY_LIST_OFFSET);
        if (!entityList) return;

        float* viewMatrix = (float*)(base + VIEW_MATRIX_OFFSET);
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);

        int localTeam = *(int*)(localPlayer + playerTeamOffset);

        for (int i = 1; i < currentPlayers; i++) {
            uintptr_t entityAddr = *(uintptr_t*)(entityList + (i * 4));
            if (!entityAddr || entityAddr == localPlayer) continue;
            if (entityAddr < 0x10000) continue;

            int health = *(int*)(entityAddr + playerHealthOffset);
            if (health <= 0 || health > 100) continue;

            int team = *(int*)(entityAddr + playerTeamOffset);
            if (team == localTeam) continue;

            Player* enemy = (Player*)entityAddr;

            Vector3 feetPos;
            feetPos.x = enemy->x;
            feetPos.y = enemy->y;
            feetPos.z = enemy->z;

            Vector3 headPos;
            headPos.x = enemy->x;
            headPos.y = enemy->y;
            headPos.z = enemy->z + 4.5f;

            Vector2 screenFeet, screenHead;
            if (!WorldToScreen(feetPos, screenFeet, viewMatrix, sw, sh)) continue;
            if (!WorldToScreen(headPos, screenHead, viewMatrix, sw, sh)) continue;

            float height = screenFeet.y - screenHead.y;
            if (height < 3.0f) continue;
            if (height > 250.0f) height = 250.0f;

            float width = height * 0.35f;
            if (width < 10.0f) width = 10.0f;
            if (width > 70.0f) width = 70.0f;

            float shiftDown = height * 0.90f;
            float boxX = screenHead.x - (width / 2);
            float boxTop = screenHead.y + shiftDown;
            float boxBottom = boxTop + height;
            float boxRight = boxX + width;

            float dist = sqrtf(
                (enemy->x - player->x) * (enemy->x - player->x) +
                (enemy->y - player->y) * (enemy->y - player->y) +
                (enemy->z - player->z) * (enemy->z - player->z)
            );
            bool isVisible = (dist < 100.0f);

            float r = isVisible ? 1.0f : 1.0f;
            float g = isVisible ? 1.0f : 0.8f;
            float b = isVisible ? 0.0f : 0.0f;

            DrawRect(boxX, boxTop, width, height, r, g, b, 0.8f, 1.5f);

            float cornerSize = width * 0.2f;
            if (cornerSize < 4.0f) cornerSize = 4.0f;
            if (cornerSize > 12.0f) cornerSize = 12.0f;

            DrawLine(boxX, boxTop, boxX + cornerSize, boxTop, 1.0f, 1.0f, 1.0f, 0.8f, 1.5f);
            DrawLine(boxX, boxTop, boxX, boxTop + cornerSize, 1.0f, 1.0f, 1.0f, 0.8f, 1.5f);
            DrawLine(boxRight, boxTop, boxRight - cornerSize, boxTop, 1.0f, 1.0f, 1.0f, 0.8f, 1.5f);
            DrawLine(boxRight, boxTop, boxRight, boxTop + cornerSize, 1.0f, 1.0f, 1.0f, 0.8f, 1.5f);
            DrawLine(boxX, boxBottom, boxX + cornerSize, boxBottom, 1.0f, 1.0f, 1.0f, 0.8f, 1.5f);
            DrawLine(boxX, boxBottom, boxX, boxBottom - cornerSize, 1.0f, 1.0f, 1.0f, 0.8f, 1.5f);
            DrawLine(boxRight, boxBottom, boxRight - cornerSize, boxBottom, 1.0f, 1.0f, 1.0f, 0.8f, 1.5f);
            DrawLine(boxRight, boxBottom, boxRight, boxBottom - cornerSize, 1.0f, 1.0f, 1.0f, 0.8f, 1.5f);

            DrawLine(sw / 2.0f, sh, screenFeet.x, screenFeet.y, r, g, b, 0.25f, 1.0f);

            DrawStr(boxX + (width / 2) - 20, boxTop - 14, 1.0f, 1.0f, 1.0f, 1.0f, "%s", enemy->name);

            DrawStr(boxX + (width / 2) - 12, boxBottom + 2, 0.6f, 0.6f, 0.6f, 0.8f, "%dm", (int)dist);

            float barX = boxX;
            float barY = boxBottom + 8;
            DrawFilledRect(barX, barY, width, 3.0f, 0.1f, 0.1f, 0.1f, 0.8f);
            float hpRatio = health / 100.0f;
            float hpR = 1.0f - hpRatio;
            DrawFilledRect(barX, barY, width * hpRatio, 3.0f, hpR, hpRatio, 0.0f, 0.9f);

            DrawStr(boxX + 2, boxTop + 2, 0.0f, 1.0f, 0.0f, 0.8f, "HP:%d", health);

            int armor = *(int*)(entityAddr + 0x158);
            if (armor > 0) {
                DrawStr(boxX + 2, boxTop + 12, 0.0f, 0.5f, 1.0f, 0.8f, "A:%d", armor);
            }

            if (*(int*)(entityAddr + playerShootingOffset) == 1) {
                DrawStr(boxRight + 2, boxTop + (height / 2) - 6, 1.0f, 1.0f, 0.0f, 1.0f, "!");
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
}

// ═══════════════════════════════════════════════════════
//  AIMBOT
// ═══════════════════════════════════════════════════════
void DoAimbot() {
    if (!bAimbot) return;

    if (!(GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
        lastTarget = 0;
        return;
    }

    uintptr_t base = GetBase();
    if (!base) return;

    uintptr_t localPlayer = GetPlayerPtrSafe();
    if (!localPlayer) return;

    float localX = *(float*)(localPlayer + 0x4);
    float localY = *(float*)(localPlayer + 0x8);
    float localZ = *(float*)(localPlayer + 0xC) + 4.5f;
    int localTeam = *(int*)(localPlayer + 0x32C);

    uintptr_t entityList = *(uintptr_t*)(base + 0x10F4F8);
    if (!entityList) return;

    int currentPlayers = *(int*)(base + 0x10F500);
    if (currentPlayers <= 0 || currentPlayers > 32) return;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    float centerX = sw / 2.0f;
    float centerY = sh / 2.0f;

    float* viewMatrix = (float*)(base + VIEW_MATRIX_OFFSET);

    float closest_player = -1.0f;
    uintptr_t closest_entity = 0;

    for (int i = 0; i < currentPlayers; i++) {
        uintptr_t entityAddr = *(uintptr_t*)(entityList + (i * 4));
        if (!entityAddr || entityAddr == localPlayer) continue;
        if (entityAddr < 0x10000) continue;

        int health = *(int*)(entityAddr + 0xF8);
        if (health <= 0 || health > 100) continue;

        int team = *(int*)(entityAddr + 0x32C);
        if (team == localTeam) continue;

        float enemyX = *(float*)(entityAddr + 0x4);
        float enemyY = *(float*)(entityAddr + 0x8);
        float enemyZ = *(float*)(entityAddr + 0xC);

        Vector3 enemyPos = { enemyX, enemyY, enemyZ };
        Vector2 screenPos;

        if (!WorldToScreen(enemyPos, screenPos, viewMatrix, sw, sh)) continue;

        float dx = screenPos.x - centerX;
        float dy = screenPos.y - centerY;
        float screenDist = sqrtf((dx * dx) + (dy * dy));

        if (screenDist > 150.0f) continue;

        float dist = sqrtf(
            (enemyX - localX) * (enemyX - localX) +
            (enemyY - localY) * (enemyY - localY) +
            (enemyZ - localZ) * (enemyZ - localZ)
        );

        if (closest_player == -1.0f || dist < closest_player) {
            closest_player = dist;
            closest_entity = entityAddr;
        }
    }

    if (!closest_entity) return;

    float targetX = *(float*)(closest_entity + 0x4);
    float targetY = *(float*)(closest_entity + 0x8);
    float targetZ = *(float*)(closest_entity + 0xC) + 4.5f;

    float deltaX = targetX - localX;
    float deltaY = targetY - localY;
    float deltaZ = targetZ - localZ;

    float length = sqrtf((deltaX * deltaX) + (deltaY * deltaY) + (deltaZ * deltaZ));
    if (length < 0.01f) return;

    float normX = deltaX / length;
    float normY = deltaY / length;
    float normZ = deltaZ / length;

    float yaw = -atan2f(normX, normY) * (180.0f / 3.14159265f) + 180.0f;
    if (yaw < 0) yaw += 360.0f;
    if (yaw >= 360.0f) yaw -= 360.0f;

    float horizontalDist = sqrtf((normX * normX) + (normY * normY));
    if (horizontalDist < 0.01f) horizontalDist = 0.01f;

    float pitch = atan2f(normZ, horizontalDist) * (180.0f / 3.14159265f);

    if (pitch > 89.0f) pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;

    *(float*)(localPlayer + 0x40) = yaw;
    *(float*)(localPlayer + 0x44) = pitch;
}

// ═══════════════════════════════════════════════════════
//  FOV CIRCLE
// ═══════════════════════════════════════════════════════
void DrawFOVCircle() {
    if (!bFOV || !bAimbot) return;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int centerX = sw / 2;
    int centerY = sh / 2;

    DrawCircle((float)centerX, (float)centerY, 150.0f, 0.0f, 1.0f, 0.0f, 0.5f, 64);

    DrawLine((float)centerX - 8, (float)centerY, (float)centerX + 8, (float)centerY, 0.0f, 1.0f, 0.0f, 0.5f);
    DrawLine((float)centerX, (float)centerY - 8, (float)centerX, (float)centerY + 8, 0.0f, 1.0f, 0.0f, 0.5f);
}

// ═══════════════════════════════════════════════════════
//  FEATURES
// ═══════════════════════════════════════════════════════
void RunFeatures() {
    uintptr_t base = GetBase();
    uintptr_t player = GetPlayerPtrSafe();

    if (player && player > 0x10000) {
        if (bGodMode)     *(int*)(player + playerHealthOffset) = 1337;
        if (bInfArmour)   *(int*)(player + 0x158) = 999;
        if (bInfGrenades) *(int*)(player + 0x0FC) = 99;

        if (bInfAmmo) {
            uintptr_t ammoAddr = mem::FindDMAAddy(base + playerBaseOffset, { 0x374, 0x14, 0x0 });
            if (ammoAddr && ammoAddr > 0x10000) {
                *(int*)ammoAddr = 1337;
            }
        }
    }

    if (bNoRecoil && !recoilNoped) {
        mem::Nop((BYTE*)(base + weaponRecoilOffset), 10);
        recoilNoped = true;
    }
    else if (!bNoRecoil && recoilNoped) {
        mem::Patch((BYTE*)(base + weaponRecoilOffset), recoilOrig, 10);
        recoilNoped = false;
    }

    if (bSpeedHack) {
        mem::Patch((BYTE*)(base + 0x5BEA0), (BYTE*)"\xB8\x03\x00\x00\x00", 5);
        mem::Patch((BYTE*)(base + 0x5BE40), (BYTE*)"\xB8\xFD\xFF\xFF\xFF", 5);
        mem::Patch((BYTE*)(base + 0x5BF00), (BYTE*)"\xB8\x03\x00\x00\x00", 5);
        mem::Patch((BYTE*)(base + 0x5BF60), (BYTE*)"\xB8\xFD\xFF\xFF\xFF", 5);
    }
    else {
        mem::Patch((BYTE*)(base + 0x5BEA0), (BYTE*)"\xB8\x01\x00\x00\x00", 5);
        mem::Patch((BYTE*)(base + 0x5BE40), (BYTE*)"\xB8\xFF\xFF\xFF\xFF", 5);
        mem::Patch((BYTE*)(base + 0x5BF00), (BYTE*)"\xB8\x01\x00\x00\x00", 5);
        mem::Patch((BYTE*)(base + 0x5BF60), (BYTE*)"\xB8\xFF\xFF\xFF\xFF", 5);
    }

    DoAimbot();
}

// ═══════════════════════════════════════════════════════
//  MENU
// ═══════════════════════════════════════════════════════
void RenderMenu() {
    const float X = 30, Y = 40;
    const float W = 320;  // WIDER)
    const float HDR = 35; // TALLER HEADER
    const float ROW = 30; // TALLER ROWS
    const float H = HDR + g_Count * ROW + 45; // MORE HEIGHT

    FillRect(X, Y, W, H, 0.06f, 0.06f, 0.10f, 0.92f);
    Border(X, Y, W, H, 1.5f, 0.25f, 0.15f, 0.45f, 1.0f);
    Border(X + 2, Y + 2, W - 4, H - 4, 1.0f, 0.08f, 0.08f, 0.15f, 0.5f);

    FillRect(X + 2, Y + 2, W - 4, HDR, 0.15f, 0.08f, 0.30f, 0.95f);
    FillRect(X + 2, Y + HDR - 1, W - 4, 2, 0.30f, 0.15f, 0.60f, 0.8f);
    DrawStr(X + 12, Y + 8, 0.85f, 0.55f, 1.0f, 1.0f, "AXIOM AC v2.0");

    uintptr_t p = GetPlayerPtrSafe();
    uintptr_t base = GetBase();
    int hp = p ? *(int*)(p + playerHealthOffset) : 0;
    int arm = p ? *(int*)(p + 0x158) : 0;
    int gren = p ? *(int*)(p + 0x0FC) : 0;
    int ammo = 0;
    if (p && base) {
        uintptr_t ammoAddr = mem::FindDMAAddy(base + playerBaseOffset, { 0x374, 0x14, 0x0 });
        if (ammoAddr && ammoAddr > 0x10000) ammo = *(int*)ammoAddr;
    }

    float statsY = Y + HDR + 4;
    FillRect(X + 4, statsY, W - 8, 26, 0.08f, 0.08f, 0.14f, 0.9f);

    DrawStr(X + 6, statsY + 4, 0.2f, 0.8f, 0.2f, 1.0f, "HP");
    FillRect(X + 28, statsY + 5, 50, 16, 0.15f, 0.15f, 0.15f, 0.8f);
    float hpRatio = hp > 0 ? (hp / 1337.0f) : 0.0f;
    if (hpRatio > 1.0f) hpRatio = 1.0f;
    float hColor[3] = { 0.0f, 1.0f, 0.0f };
    if (hpRatio < 0.5f) { hColor[0] = 1.0f; hColor[1] = hpRatio * 2; }
    if (hpRatio < 0.25f) { hColor[0] = 1.0f; hColor[1] = 0.0f; hColor[2] = 0.0f; }
    FillRect(X + 28, statsY + 5, 50 * hpRatio, 16, hColor[0], hColor[1], hColor[2], 0.9f);
    DrawStr(X + 38, statsY + 4, 0.7f, 0.7f, 0.7f, 0.6f, "%d", hp > 1337 ? 1337 : hp);

    DrawStr(X + 86, statsY + 4, 0.2f, 0.6f, 0.9f, 1.0f, "ARM");
    FillRect(X + 118, statsY + 5, 50, 16, 0.15f, 0.15f, 0.15f, 0.8f);
    float armRatio = arm > 0 ? (arm / 999.0f) : 0.0f;
    if (armRatio > 1.0f) armRatio = 1.0f;
    FillRect(X + 118, statsY + 5, 50 * armRatio, 16, 0.2f, 0.5f, 0.9f, 0.9f);
    DrawStr(X + 128, statsY + 4, 0.7f, 0.7f, 0.7f, 0.6f, "%d", arm > 999 ? 999 : arm);

    DrawStr(X + 176, statsY + 4, 0.9f, 0.9f, 0.2f, 1.0f, "AMMO");
    DrawStr(X + 220, statsY + 4, 0.7f, 0.7f, 0.7f, 0.9f, "%d", ammo);
    DrawStr(X + 262, statsY + 4, 0.9f, 0.2f, 0.9f, 1.0f, "GREN");
    DrawStr(X + 288, statsY + 4, 0.7f, 0.7f, 0.7f, 0.9f, "%d", gren);

    for (int i = 0; i < g_Count; i++) {
        float ry = Y + HDR + 34 + i * ROW;
        bool selected = (i == g_Sel);
        bool enabled = *g_Items[i].val;

        if (selected) {
            FillRect(X + 2, ry, W - 4, ROW, 0.15f, 0.08f, 0.35f, 0.4f);
            FillRect(X + 2, ry, 4, ROW, 0.40f, 0.15f, 0.80f, 0.7f);
        }

        float lc = selected ? 1.0f : 0.75f;
        DrawStr(X + 14, ry + 6, lc, lc, lc, 1.0f, "%s", g_Items[i].label);

        float tx = X + W - 56;
        float ty = ry + 5;
        FillRect(tx, ty, 46, 20, 0.12f, 0.12f, 0.12f, 0.8f);
        Border(tx, ty, 46, 20, 1, 0.2f, 0.2f, 0.2f, 0.5f);

        if (enabled) {
            FillRect(tx + 2, ty + 2, 42, 16, 0.0f, 0.55f, 0.0f, 0.85f);
            DrawStr(tx + 10, ty + 2, 0.9f, 0.9f, 0.9f, 0.9f, "ON");
        }
        else {
            FillRect(tx + 2, ty + 2, 42, 16, 0.40f, 0.0f, 0.0f, 0.70f);
            DrawStr(tx + 9, ty + 2, 0.5f, 0.5f, 0.5f, 0.8f, "OFF");
        }

        if (selected) {
            DrawStr(X + 3, ry + 6, 0.5f, 0.2f, 1.0f, 0.8f, ">");
        }
    }

    float footY = Y + H - 24;
    FillRect(X + 4, footY, W - 8, 20, 0.04f, 0.04f, 0.08f, 0.7f);
    DrawStr(X + 10, footY + 3, 0.4f, 0.4f, 0.4f, 0.6f,
        "INS:Menu  UP/DN:Nav  ENTER:Toggle  END:Unload");
}

// ═══════════════════════════════════════════════════════
//  SWAPBUFFERS HOOK
// ═══════════════════════════════════════════════════════
BOOL __stdcall hkSwapBuffers(HDC hdc) {
    InitFont(hdc);
    RunFeatures();

    int w, h;
    GetVP(w, h);
    Begin2D(w, h);

    DrawFOVCircle();
    DrawESP();

    if (g_Menu) {
        RenderMenu();
    }

    End2D();

    return oSwapBuffers(hdc);
}

void HookSwapBuffers() {
    HMODULE hGL = GetModuleHandleA("opengl32.dll");
    if (!hGL) return;

    FARPROC target = GetProcAddress(hGL, "wglSwapBuffers");
    if (!target) return;

    static BYTE trampoline[10];
    memcpy(trampoline, (void*)target, 5);
    BYTE* jmpBack = trampoline + 5;
    jmpBack[0] = 0xE9;
    DWORD back = (DWORD)((BYTE*)target + 5) - (DWORD)(jmpBack + 5);
    memcpy(jmpBack + 1, &back, 4);
    DWORD tmp;
    VirtualProtect(trampoline, 10, PAGE_EXECUTE_READWRITE, &tmp);
    oSwapBuffers = reinterpret_cast<twglSwapBuffers>((void*)trampoline);

    BYTE patch[5]; patch[0] = 0xE9;
    DWORD rel = (DWORD)hkSwapBuffers - (DWORD)target - 5;
    memcpy(patch + 1, &rel, 4);
    DWORD old;
    VirtualProtect((LPVOID)target, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((LPVOID)target, patch, 5);
    VirtualProtect((LPVOID)target, 5, old, &old);
}

// ═══════════════════════════════════════════════════════
//  INPUT THREAD
// ═══════════════════════════════════════════════════════
DWORD WINAPI InputThread(LPVOID) {
    while (!(GetAsyncKeyState(VK_END) & 1)) {
        Sleep(60);
        if (GetAsyncKeyState(VK_INSERT) & 1) g_Menu = !g_Menu;
        if (!g_Menu) continue;
        if (GetAsyncKeyState(VK_DOWN) & 1) g_Sel = (g_Sel + 1) % g_Count;
        if (GetAsyncKeyState(VK_UP) & 1) g_Sel = (g_Sel - 1 + g_Count) % g_Count;
        if (GetAsyncKeyState(VK_RETURN) & 1) *g_Items[g_Sel].val = !*g_Items[g_Sel].val;
    }

    uintptr_t base = GetBase();
    if (recoilNoped)
        mem::Patch((BYTE*)(base + weaponRecoilOffset), recoilOrig, 10);
    mem::Patch((BYTE*)(base + 0x5BEA0), (BYTE*)"\xB8\x01\x00\x00\x00", 5);
    mem::Patch((BYTE*)(base + 0x5BE40), (BYTE*)"\xB8\xFF\xFF\xFF\xFF", 5);
    mem::Patch((BYTE*)(base + 0x5BF00), (BYTE*)"\xB8\x01\x00\x00\x00", 5);
    mem::Patch((BYTE*)(base + 0x5BF60), (BYTE*)"\xB8\xFF\xFF\xFF\xFF", 5);

    FreeLibraryAndExitThread(g_hMod, 0);
    return 0;
}

// ═══════════════════════════════════════════════════════
//  MAIN + DLLMAIN
// ═══════════════════════════════════════════════════════
DWORD WINAPI MainThread(LPVOID) {
    while (!GetModuleHandleA("ac_client.exe")) Sleep(100);
    Sleep(300);

    moduleBase = GetBase();
    HookSwapBuffers();
    CreateThread(nullptr, 0, InputThread, nullptr, 0, nullptr);

    // ─── MESSAGE BOX ON INJECTION ─────────────────────
    MessageBoxA(NULL,
        "AXIOM CHEAT INJECTED SUCCESSFULLY!\n\n"
        "Press INSERT to toggle menu.\n"
        "Press END to unload the cheat.",
        "AXIOM AC v2.0 - Injected",
        MB_OK | MB_ICONINFORMATION);

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hMod = hMod;
        DisableThreadLibraryCalls(hMod);
        CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    }
    return TRUE;
}