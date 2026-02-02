#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <vector>
#include <algorithm>
#include <array>
#include <sys/mman.h>

#include "pl/Hook.h"
#include "pl/Gloss.h"

#include "ImGui/imgui.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"

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

static bool g_PatchesReady = false;
static std::vector<uintptr_t> g_PatchAddrs;
static std::vector<std::array<uint8_t,4>> g_Originals;

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
                std::array<uint8_t,4> orig;
                memcpy(orig.data(), (void*)addr, 4);
                g_Originals.push_back(orig);
            }
        }
    }
    g_PatchesReady = true;
}

static bool g_initialized = false;
static int g_width = 0, g_height = 0;
static EGLContext g_targetcontext = EGL_NO_CONTEXT;
static EGLSurface g_targetsurface = EGL_NO_SURFACE;
static EGLBoolean (*orig_eglswapbuffers)(EGLDisplay, EGLSurface) = nullptr;
static void (*orig_input1)(void*, void*, void*) = nullptr;
static int32_t (*orig_input2)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;

static void hook_input1(void* thiz, void* a1, void* a2) {
    if (orig_input1) orig_input1(thiz, a1, a2);
    if (thiz && g_initialized) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}

static int32_t hook_input2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t result = orig_input2 ? orig_input2(thiz, a1, a2, a3, a4, event) : 0;
    if (result == 0 && event && *event && g_initialized) ImGui_ImplAndroid_HandleInputEvent(*event);
    return result;
}

static void DrawMenu() {
    ImGui::SetNextWindowPos(ImVec2(10, 80), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(350, 250), ImGuiCond_FirstUseEver);
    ImGui::Begin("AnarchyArray", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    static bool infinitySpread = false;
    static bool spongePlus = false;
    static bool spongePlusPlus = false;
    static int absorbTypeVal = 5;

    if (ImGui::Checkbox("InfinitySpread", &infinitySpread) && g_PatchesReady) {
        const uint8_t patch[] = {0x03, 0x00, 0x80, 0x52};
        for (size_t i = 0; i < 4 && i < g_PatchAddrs.size(); i++) {
            WriteMemory((void*)g_PatchAddrs[i], infinitySpread ? (void*)patch : (void*)g_Originals[i].data(), 4, true);
        }
    }

    if (ImGui::Checkbox("SpongeRange+", &spongePlus) && g_PatchesReady) {
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

    if (ImGui::Checkbox("SpongeRange++", &spongePlusPlus) && g_PatchesReady) {
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

    ImGui::Text("Absorb Type"); ImGui::SameLine();
    ImGui::SetNextItemWidth(50);
    ImGui::InputInt("##absorbDisplay", &absorbTypeVal, 0, 0, ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    
    if (ImGui::Button("K", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
        ImGui::OpenPopup("AbsorbKeypad");
    }
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
    ImGui::SameLine();
    if (ImGui::Button("-", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
        if (absorbTypeVal > 0) absorbTypeVal--;
    }
    ImGui::SameLine();
    if (ImGui::Button("+", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
        if (absorbTypeVal < 575) absorbTypeVal++;
    }
    ImGui::PopStyleVar(3);

    if (g_PatchesReady && absorbTypeVal >= 0 && absorbTypeVal <= 575) {
        for (size_t idx : {6, 7}) {
            if (idx < g_PatchAddrs.size()) {
                uint32_t instr = EncodeCmpW8Imm_Table(absorbTypeVal);
                if (instr != 0) {
                    WriteMemory((void*)g_PatchAddrs[idx], &instr, 4, true);
                }
            }
        }
    }

    if (ImGui::BeginPopup("AbsorbKeypad", ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Keypad");
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight());
        if (ImGui::Button("X", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::Separator();
        
        const float cellWidth = 60.0f;
        const float rowHeight = 50.0f;
        
        for (int i = 1; i <= 3; i++) {
            if (ImGui::Button(std::to_string(i).c_str(), ImVec2(cellWidth, rowHeight))) {
                absorbTypeVal = absorbTypeVal * 10 + i;
            }
            if (i < 3) ImGui::SameLine();
        }
        
        for (int i = 4; i <= 6; i++) {
            if (ImGui::Button(std::to_string(i).c_str(), ImVec2(cellWidth, rowHeight))) {
                absorbTypeVal = absorbTypeVal * 10 + i;
            }
            if (i < 6) ImGui::SameLine();
        }
        
        for (int i = 7; i <= 9; i++) {
            if (ImGui::Button(std::to_string(i).c_str(), ImVec2(cellWidth, rowHeight))) {
                absorbTypeVal = absorbTypeVal * 10 + i;
            }
            if (i < 9) ImGui::SameLine();
        }
        
        ImGui::Dummy(ImVec2(cellWidth, rowHeight));
        ImGui::SameLine();
        if (ImGui::Button("0", ImVec2(cellWidth, rowHeight))) {
            absorbTypeVal = absorbTypeVal * 10;
        }
        ImGui::SameLine();
        if (ImGui::Button("<-", ImVec2(cellWidth, rowHeight))) {
            absorbTypeVal /= 10;
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

static void setup() {
    if (g_initialized || g_width <= 0) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.FontGlobalScale = 1.4f;
    ImGui_ImplAndroid_Init();
    ImGui_ImplOpenGL3_Init("#version 300 es");
    g_initialized = true;
}

static void render() {
    if (!g_initialized) return;

    if (!g_PatchesReady) {
        ScanSignatures();
    }

    GLint last_prog; glGetIntegerv(GL_CURRENT_PROGRAM, &last_prog);
    GLint last_tex; glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex);
    GLint last_array_buffer; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);
    GLint last_element_array_buffer; glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_element_array_buffer);
    GLint last_fbo; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);
    GLint last_viewport[4]; glGetIntegerv(GL_VIEWPORT, last_viewport);
    GLboolean last_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean last_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean last_blend = glIsEnabled(GL_BLEND);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_width, (float)g_height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(g_width, g_height);
    ImGui::NewFrame();
    
    DrawMenu();
    
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glUseProgram(last_prog);
    glBindTexture(GL_TEXTURE_2D, last_tex);
    glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, last_element_array_buffer);
    glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
    glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
    if (last_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (last_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (last_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
}

static EGLBoolean hook_eglswapbuffers(EGLDisplay dpy, EGLSurface surf) {
    if (!orig_eglswapbuffers) return EGL_FALSE;
    EGLContext ctx = eglGetCurrentContext();
    if (ctx == EGL_NO_CONTEXT || (g_targetcontext != EGL_NO_CONTEXT && (ctx != g_targetcontext || surf != g_targetsurface)))
        return orig_eglswapbuffers(dpy, surf);
    
    EGLint w, h;
    eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    if (w < 100 || h < 100) return orig_eglswapbuffers(dpy, surf);

    if (g_targetcontext == EGL_NO_CONTEXT) { g_targetcontext = ctx; g_targetsurface = surf; }
    g_width = w; g_height = h;
    
    setup();
    render();
    
    return orig_eglswapbuffers(dpy, surf);
}

static void hookinput() {
    void* sym = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE", nullptr);
    if (sym) GlossHook(sym, (void*)hook_input2, (void**)&orig_input2);
    
    void* sym2 = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (sym2) GlossHook(sym2, (void*)hook_input1, (void**)&orig_input1);
}

static void* mainthread(void*) {
    sleep(3);
    GlossInit(true);
    GHandle hegl = GlossOpen("libEGL.so");

    if (hegl) {
        void* swap = (void*)GlossSymbol(hegl, "eglSwapBuffers", nullptr);
        if (swap) GlossHook(swap, (void*)hook_eglswapbuffers, (void**)&orig_eglswapbuffers);
    }

    hookinput();
    return nullptr;
}

__attribute__((constructor))
void display_init() {
    pthread_t t;
    pthread_create(&t, nullptr, mainthread, nullptr);
}
