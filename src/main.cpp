#include <jni.h>
#include <android/input.h>
#include <android/native_window.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <vector>
#include <array>
#include <cstring>
#include <string>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

static bool g_Initialized = false;
static ANativeWindow* g_Window = nullptr;
static int g_Width = 0, g_Height = 0;
static EGLContext g_TargetContext = EGL_NO_CONTEXT;
static EGLSurface g_TargetSurface = EGL_NO_SURFACE;

static bool g_PatchesReady = false;
static std::vector<uintptr_t> g_PatchAddrs;
static std::vector<std::array<uint8_t, 4>> g_Originals;

static void WriteMemory(void* dest, const void* src, size_t size, bool protect) {
    if (protect) {
        uintptr_t page_size = sysconf(_SC_PAGESIZE);
        uintptr_t addr = (uintptr_t)dest;
        uintptr_t page_addr = addr & ~(page_size - 1);
        mprotect((void*)page_addr, page_size, PROT_READ | PROT_WRITE | PROT_EXEC);
    }
    memcpy(dest, src, size);
    if (protect) {
        uintptr_t page_size = sysconf(_SC_PAGESIZE);
        uintptr_t addr = (uintptr_t)dest;
        uintptr_t page_addr = addr & ~(page_size - 1);
        mprotect((void*)page_addr, page_size, PROT_READ | PROT_EXEC);
    }
    __builtin___clear_cache((char*)dest, (char*)dest + size);
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

static void ScanSignatures() {
    if (g_PatchesReady) return;
    uintptr_t base = GlossGetLibSection("libminecraftpe.so", ".text", nullptr);
    size_t size = 0;
    GlossGetLibSection("libminecraftpe.so", ".text", &size);

    if (base == 0 || size == 0) return;

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

    for (auto& sig : signatures) {
        for (size_t i = 0; i + sig.size() < size; i++) {
            if (!memcmp((void*)(base + i), sig.data(), sig.size())) {
                uintptr_t addr = base + i;
                g_PatchAddrs.push_back(addr);
                std::array<uint8_t, 4> orig;
                memcpy(orig.data(), (void*)addr, 4);
                g_Originals.push_back(orig);
            }
        }
    }
    g_PatchesReady = true;
}

static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static EGLSurface (*orig_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*) = nullptr;
static ANativeWindow* (*orig_ANativeWindow_fromSurface)(JNIEnv*, jobject) = nullptr;

static void (*orig_Input1)(void*, void*, void*) = nullptr;
static void hook_Input1(void* thiz, void* a1, void* a2) {
    if (orig_Input1) orig_Input1(thiz, a1, a2);
    if (thiz && g_Initialized) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}

static int32_t (*orig_Input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;
static int32_t hook_Input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t result = orig_Input2 ? orig_Input2(thiz, a1, a2, a3, a4, event) : 0;
    if (result == 0 && event && *event && g_Initialized) {
        ImGui_ImplAndroid_HandleInputEvent(*event);
    }
    return result;
}

struct GLState {
    GLint prog, tex, aTex, aBuf, eBuf, vao, fbo, vp[4], sc[4], bSrc, bDst;
    GLboolean blend, cull, depth, scissor;
};

static void SaveGL(GLState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.prog);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s.tex);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.aTex);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.aBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.eBuf);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.fbo);
    glGetIntegerv(GL_VIEWPORT, s.vp);
    glGetIntegerv(GL_SCISSOR_BOX, s.sc);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.bSrc);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.bDst);
    s.blend = glIsEnabled(GL_BLEND);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
}

static void RestoreGL(const GLState& s) {
    glUseProgram(s.prog);
    glActiveTexture(s.aTex);
    glBindTexture(GL_TEXTURE_2D, s.tex);
    glBindBuffer(GL_ARRAY_BUFFER, s.aBuf);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s.eBuf);
    glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbo);
    glViewport(s.vp[0], s.vp[1], s.vp[2], s.vp[3]);
    glScissor(s.sc[0], s.sc[1], s.sc[2], s.sc[3]);
    glBlendFunc(s.bSrc, s.bDst);
    s.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    s.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    s.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    s.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
}

static void DrawMenu() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("AnarchyArray", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    
    ImGui::Text("FPS: %.1f", io.Framerate);
    ImGui::Separator();

    static bool infinitySpread = false;
    static bool spongePlus = false;
    static bool spongePlusPlus = false;
    static bool sponge3x3 = false; 

    if (!g_PatchesReady) {
        ImGui::Text("Scanning...");
    } else {
        if (ImGui::Checkbox("Infinity Spread", &infinitySpread)) {
            const uint8_t patch[] = {0x03, 0x00, 0x80, 0x52};
            for (size_t i = 0; i < 4 && i < g_PatchAddrs.size(); i++) {
                WriteMemory((void*)g_PatchAddrs[i], infinitySpread ? (void*)patch : (void*)g_Originals[i].data(), 4, true);
            }
        }

        if (ImGui::Checkbox("Sponge Range+", &spongePlus)) {
            const uint8_t patchPlus[] = {0x1F, 0x20, 0x03, 0xD5, 0xFB, 0x13, 0x40, 0xF9, 0x7F, 0x07, 0x00, 0xB1};
            size_t idx = 4;
            if (idx < g_PatchAddrs.size()) {
                if (spongePlus) {
                    WriteMemory((void*)g_PatchAddrs[idx], (void*)patchPlus, sizeof(patchPlus), true);
                } else {
                    WriteMemory((void*)g_PatchAddrs[idx], (void*)g_Originals[idx].data(), 4, true);
                }
            }
        }

        if (ImGui::Checkbox("Sponge Range++", &spongePlusPlus)) {
            const uint8_t patchPlusPlus[] = {0x5F, 0xFD, 0x03, 0xF1, 0x8B, 0x2D, 0x0D, 0x9B};
            size_t idx = 5;
            if (idx < g_PatchAddrs.size()) {
                if (spongePlusPlus) {
                    WriteMemory((void*)g_PatchAddrs[idx], (void*)patchPlusPlus, sizeof(patchPlusPlus), true);
                    spongePlus = true;
                } else {
                    WriteMemory((void*)g_PatchAddrs[idx], (void*)g_Originals[idx].data(), 4, true);
                }
            }
        }

        if (ImGui::Checkbox("Sponge 3x3 (Weak)", &sponge3x3)) {
            uint32_t val = sponge3x3 ? EncodeCmpW8Imm_Table(1) : EncodeCmpW8Imm_Table(5);
            if (val != 0) {
                for (size_t idx : {6, 7}) {
                    if (idx < g_PatchAddrs.size()) {
                        WriteMemory((void*)g_PatchAddrs[idx], &val, 4, true);
                    }
                }
            }
        }
    }

    ImGui::End();
}

static void Setup() {
    if (g_Initialized || g_Width <= 0 || g_Height <= 0 || !g_Window) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    float scale = (float)g_Height / 720.0f;
    if (scale < 1.5f) scale = 1.5f;
    if (scale > 4.0f) scale = 4.0f;
    ImFontConfig cfg;
    cfg.SizePixels = 28.0f * scale;
    io.Fonts->AddFontDefault(&cfg);
    ImGui_ImplAndroid_Init(g_Window);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    ImGui::GetStyle().ScaleAllSizes(scale);
    g_Initialized = true;
}

static void Render() {
    if (!g_Initialized) return;
    
    if (!g_PatchesReady) {
        ScanSignatures();
    }

    GLState s;
    SaveGL(s);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();
    DrawMenu();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    RestoreGL(s);
}

static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglSwapBuffers) return EGL_FALSE;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT) return orig_eglSwapBuffers(dpy, surf);
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 500 || h < 500) return orig_eglSwapBuffers(dpy, surf);
    if (g_TargetContext == EGL_NO_CONTEXT) {
        EGLint buf = 0;
        eglQuerySurface(dpy, surf, EGL_RENDER_BUFFER, &buf);
        if (buf == EGL_BACK_BUFFER) {
            g_TargetContext = ctx;
            g_TargetSurface = surf;
        }
    }
    if (ctx != g_TargetContext || surf != g_TargetSurface)
        return orig_eglSwapBuffers(dpy, surf);
    g_Width = w;
    g_Height = h;
    Setup();
    Render();
    return orig_eglSwapBuffers(dpy, surf);
}

static EGLSurface hook_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint* attrib_list) {
    if (win) g_Window = (ANativeWindow*)win;
    return orig_eglCreateWindowSurface ? orig_eglCreateWindowSurface(dpy, config, win, attrib_list) : EGL_NO_SURFACE;
}

static ANativeWindow* hook_ANativeWindow_fromSurface(JNIEnv* env, jobject surface) {
    ANativeWindow* win = orig_ANativeWindow_fromSurface(env, surface);
    if (win) g_Window = win;
    return win;
}

static void HookInput() {
    void* sym1 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (sym1) {
        GHook h = GlossHook(sym1, (void*)hook_Input1, (void**)&orig_Input1);
        if (h) return;
    }
    void* sym2 = (void*)GlossSymbol(GlossOpen("libinput.so"),
        "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE", nullptr);
    if (sym2) {
        GHook h = GlossHook(sym2, (void*)hook_Input2, (void**)&orig_Input2);
        if (h) return;
    }
}

static void* MainThread(void*) {
    sleep(3);
    GlossInit(true);
    GHandle hEGL = GlossOpen("libEGL.so");
    if (!hEGL) return nullptr;
    
    void* swap = (void*)GlossSymbol(hEGL, "eglSwapBuffers", nullptr);
    if (swap) GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
    
    void* create = (void*)GlossSymbol(hEGL, "eglCreateWindowSurface", nullptr);
    if (create) GlossHook(create, (void*)hook_eglCreateWindowSurface, (void**)&orig_eglCreateWindowSurface);

    GHandle hAndroid = GlossOpen("libandroid.so");
    if (hAndroid) {
        void* fromSurf = (void*)GlossSymbol(hAndroid, "ANativeWindow_fromSurface", nullptr);
        if (fromSurf) GlossHook(fromSurf, (void*)hook_ANativeWindow_fromSurface, (void**)&orig_ANativeWindow_fromSurface);
    }

    HookInput();
    return nullptr;
}

__attribute__((constructor))
void AnarchyArray_Init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
