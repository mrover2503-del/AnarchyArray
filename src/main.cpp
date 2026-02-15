#include <cstdint>
#include <cstdlib>
#include <vector>
#include <array>
#include <string>

#include <jni.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_window.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <dlfcn.h>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "AnarchyArray", __VA_ARGS__)

static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;

static ANativeWindow* g_Window = nullptr;
static ANativeWindow* (*orig_ANativeWindow_fromSurface)(JNIEnv* env, jobject surface) = nullptr;
static EGLBoolean (*orig_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static EGLSurface (*orig_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*) = nullptr;

static bool g_PatchesReady = false;
static std::vector<uintptr_t> g_PatchAddrs;
static std::vector<std::array<uint8_t,4>> g_Originals;

// InputConsumer::initializeMotionEvent
static void (*initMotionEvent)(void*, void*, void*) = nullptr;
static void HookInput1(void* thiz, void* a1, void* a2) {
    if (initMotionEvent) initMotionEvent(thiz, a1, a2);
    if (thiz && g_Initialized) {
        ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
    }
}

// InputConsumer::Consume
static int32_t (*Consume)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;
static int32_t HookInput2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t result = Consume ? Consume(thiz, a1, a2, a3, a4, event) : 0;
    if (result == 0 && event && *event && g_Initialized) {
        ImGui_ImplAndroid_HandleInputEvent(*event);
    }
    return result;
}

struct GLState {
    GLint program;
    GLint vao;
    GLint fbo;
    GLint viewport[4];
    GLint scissor[4];
    GLboolean blend;
    GLboolean scissorTest;
};

static void SaveGL(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    glGetIntegerv(GL_SCISSOR_BOX, s.scissor);
    s.blend = glIsEnabled(GL_BLEND);
    s.scissorTest = glIsEnabled(GL_SCISSOR_TEST);
}

static void RestoreGL(const GLState& s) {
    glUseProgram(s.program);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    glScissor(s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3]);
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.scissorTest ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

static uint32_t EncodeCmpW8Imm_Table(int imm) { 
    if (imm < 0 || imm > 575) return 0;
    uint32_t instr = 0x7100001F;
    int block = imm / 64;
    int offset = imm % 64;
    uint8_t immByte = 0x01 + (offset * 0x04);
    uint8_t* p = reinterpret_cast<uint8_t*>(&instr);
    p[1] = immByte;
    p[2] = (uint8_t)block;
    return instr;
}

static void DrawMenu() {
    ImGui::Begin("AnarchyArray", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize);
    static bool infinitySpread = false;
    static bool spongePlus = false;
    static bool spongePlusPlus = false;
    static int absorbTypeVal = 5;

    // InfinitySpread
    if (ImGui::Checkbox("InfinitySpread", &infinitySpread) && g_PatchesReady) {
        const uint8_t patch[] = {0x03, 0x00, 0x80, 0x52};
        for (size_t i = 0; i < 4 && i < g_PatchAddrs.size(); i++) {
            if (g_PatchAddrs[i] != 0) { // NULL 주소 체크 추가
                WriteMemory((void*)g_PatchAddrs[i], infinitySpread ? (void*)patch : (void*)g_Originals[i].data(), 4, true);
            }
        }
    }

    // SpongeRange+
    if (ImGui::Checkbox("SpongeRange+", &spongePlus) && g_PatchesReady) {
        const uint8_t patchPlus[] = {0x1F, 0x20, 0x03, 0xD5, 0xFB, 0x13, 0x40, 0xF9, 0x7F, 0x07, 0x00, 0xB1};
        size_t idx = 4;
        if (idx < g_PatchAddrs.size() && g_PatchAddrs[idx] != 0) {
            WriteMemory((void*)g_PatchAddrs[idx], spongePlus ? (void*)patchPlus : (void*)g_Originals[idx].data(), spongePlus ? sizeof(patchPlus) : 4, true);
        }
    }

    // SpongeRange++
    ImGui::BeginDisabled(!spongePlus); 
    if (ImGui::Checkbox("SpongeRange++", &spongePlusPlus) && g_PatchesReady) {
        const uint8_t patchPlusPlus[] = {0x5F, 0xFD, 0x03, 0xF1, 0x8B, 0x2D, 0x0D, 0x9B};
        size_t idx = 5;
        if (idx < g_PatchAddrs.size() && g_PatchAddrs[idx] != 0) {
            WriteMemory((void*)g_PatchAddrs[idx], spongePlusPlus ? (void*)patchPlusPlus : (void*)g_Originals[idx].data(), spongePlusPlus ? sizeof(patchPlusPlus) : 4, true);
        }
    }
    ImGui::EndDisabled();

    // Absorb Type UI 로직
    ImGui::Text("Absorb Type");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(50);
    ImGui::InputInt("##absorbDisplay", &absorbTypeVal, 0, 0, ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    
    if (ImGui::Button("K", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) ImGui::OpenPopup("AbsorbKeypad");
    ImGui::SameLine();
    if (ImGui::Button("i", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) ImGui::OpenPopup("AbsorbTypeInfo");
    ImGui::SameLine();
    if (ImGui::Button("-", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())) && absorbTypeVal > 0) absorbTypeVal--;
    ImGui::SameLine();
    if (ImGui::Button("+", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())) && absorbTypeVal < 575) absorbTypeVal++;
    
    ImGui::PopStyleVar(3);

    // Apply patch when value changes
    if (g_PatchesReady && absorbTypeVal >= 0 && absorbTypeVal <= 575) {
        for (size_t idx : {6, 7}) {
            if (idx < g_PatchAddrs.size() && g_PatchAddrs[idx] != 0) {
                uint32_t instr = EncodeCmpW8Imm_Table(absorbTypeVal);
                if (instr != 0) {
                    WriteMemory((void*)g_PatchAddrs[idx], &instr, 4, true);
                }
            }
        }
    }

    // Info Popup 생략 (기존 코드와 동일하므로 지면상 유지했다고 가정)
    if (ImGui::BeginPopup("AbsorbTypeInfo", ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Info placeholder...");
        if (ImGui::Button("X")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    
    // Keypad Popup 생략 (기존 코드와 동일)
    if (ImGui::BeginPopup("AbsorbKeypad", ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Keypad placeholder...");
        if (ImGui::Button("X")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::End();
}

static void ScanSignatures() {
    uintptr_t base = GlossGetLibSection("libminecraftpe.so", ".text", nullptr);
    size_t size = 0;
    while ((base = GlossGetLibSection("libminecraftpe.so", ".text", &size)) == 0 || size == 0) {
        usleep(10000); // 부하를 줄이기 위해 10ms로 변경
    }
    
    const std::vector<std::vector<uint8_t>> signatures = {
        {0xE3,0x03,0x19,0x2A,0xE4,0x03,0x14,0xAA,0xA5,0x00,0x80,0x52,0x08,0x05,0x00,0x51},
        {0xE3,0x03,0x19,0x2A,0x29,0x05,0x00,0x51,0xE4,0x03,0x14,0xAA,0x65,0x00,0x80,0x52},
        {0xE3,0x03,0x19,0x2A,0xE4,0x03,0x14,0xAA,0x85,0x00,0x80,0x52,0x08,0x05,0x00,0x11},
        {0xE3,0x03,0x19,0x2A,0x29,0x05,0x00,0x11,0xE4,0x03,0x14,0xAA,0x45,0x00,0x80,0x52},
        {0x62,0x02,0x00,0x54,0xFB,0x13,0x40,0xF9,0x7F,0x17,0x00,0xF1},
        {0x5F,0x51,0x05,0xF1,0x8B,0x2D,0x0D,0x9B},
        {0x1F,0x15,0x00,0x71,0xA1,0x01,0x00,0x54,0x00,0xE4,0x00,0x6F},
        {0x1F,0x15,0x00,0x71,0x01,0xF8,0xFF,0x54,0x88,0x02,0x40,0xF9},
    };

    // 중요: 시그니처 갯수만큼 미리 할당하여 인덱스가 꼬이지 않게 함
    g_PatchAddrs.assign(signatures.size(), 0);
    g_Originals.assign(signatures.size(), {0, 0, 0, 0});

    for (size_t s = 0; s < signatures.size(); s++) {
        const auto& sig = signatures[s];
        for (size_t i = 0; i + sig.size() < size; i++) {
            if (memcmp((void*)(base + i), sig.data(), sig.size()) == 0) {
                g_PatchAddrs[s] = base + i;
                memcpy(g_Originals[s].data(), (void*)(base + i), 4);
                // 중요: 하나 찾았으면 다음 시그니처로 넘어가서 중복 추가 방지!
                break; 
            }
        }
    }
    g_PatchesReady = true;
    LOGI("Signatures scanned successfully.");
}

static void Setup(ANativeWindow* window) {
    if (g_Initialized || !window) return;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
    
    float scale = (float)g_Height / 720.0f;
    if (scale < 1.5f) scale = 1.5f;
    if (scale > 4.0f) scale = 4.0f;
    
    ImFontConfig cfg;
    cfg.SizePixels = 18.0f * scale;
    io.Fonts->AddFontDefault(&cfg);
    
    ImGui_ImplAndroid_Init(window);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale * 0.65f);
    style.Alpha = 1.0f;
    
    g_Initialized = true;
    LOGI("ImGui initialized successfully");
}

static void Render() {
    if (!g_Initialized) return;
    static int lastW = 0, lastH = 0;
    ImGuiIO& io = ImGui::GetIO();
    if (g_Width != lastW || g_Height != lastH) {
        io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
        lastW = g_Width;
        lastH = g_Height;
    }
    GLState gl;
    SaveGL(gl);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();
    DrawMenu();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    RestoreGL(gl);
}

static EGLSurface hook_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint* attrib_list) {
    if (win) g_Window = (ANativeWindow*)win; // 윈도우 포인터만 저장하고 초기화는 SwapBuffers로 위임
    return orig_eglCreateWindowSurface(dpy, config, win, attrib_list);
}

static ANativeWindow* hook_ANativeWindow_fromSurface(JNIEnv* env, jobject surface) {
    ANativeWindow* win = orig_ANativeWindow_fromSurface(env, surface);
    if (win) g_Window = win;
    return win;
}

static EGLBoolean hook_eglMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
    return orig_eglMakeCurrent(dpy, draw, read, ctx);
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;
    
    EGLContext ctx = eglGetCurrentContext();
    if (ctx != EGL_NO_CONTEXT && surf != EGL_NO_SURFACE) {
        // 해상도 업데이트
        eglQuerySurface(dpy, surf, EGL_WIDTH, &g_Width);
        eglQuerySurface(dpy, surf, EGL_HEIGHT, &g_Height);

        // 컨텍스트가 활성화된 현재 위치에서 ImGui 최초 1회 초기화
        if (!g_Initialized && g_Window) {
            Setup(g_Window);
        }

        if (g_Initialized) {
            Render();
        }
    }
    
    return orig_eglSwapBuffers(dpy, surf);
}

// HookInput, MainThread, Init 함수는 기존 코드 유지
static void HookInput() { /* 기존 내용 */ }
static void* MainThread(void*) { /* 기존 내용 */ return nullptr;}
__attribute__((constructor))
void AnarchyArray_Init() { /* 기존 내용 */ }
