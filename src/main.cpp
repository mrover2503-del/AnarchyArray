#include <cstdint>
#include <cstdlib>
#include <vector>
#include <array>
#include <string>
#include <mutex>

#include <jni.h>
#include <android/input.h>
#include <android/log.h>
#include <android/native_window.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
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

// ==========================================
// [1] 전역 변수 및 상태
// ==========================================
static bool g_Initialized = false;
static int g_Width = 0, g_Height = 0;
static ANativeWindow* g_Window = nullptr;

static ANativeWindow* (*orig_ANativeWindow_fromSurface)(JNIEnv* env, jobject surface) = nullptr;
static EGLBoolean (*orig_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
static EGLSurface (*orig_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint*) = nullptr;

// 마인크래프트 패치 관련
static bool g_PatchesReady = false;
static std::vector<uintptr_t> g_PatchAddrs;
static std::vector<std::array<uint8_t,4>> g_Originals;

// ==========================================
// [2] Motion Blur 쉐이더 및 리소스
// ==========================================
const char* vertexShaderSource = R"(
attribute vec4 aPosition;
attribute vec2 aTexCoord;
varying vec2 vTexCoord;
void main() {
    gl_Position = aPosition;
    vTexCoord = aTexCoord;
}
)";

const char* blendFragmentShaderSource = R"(
precision mediump float;
varying vec2 vTexCoord;
uniform sampler2D uCurrentFrame;
uniform sampler2D uHistoryFrame;
uniform float uBlendFactor;
void main() {
    vec4 current = texture2D(uCurrentFrame, vTexCoord);
    vec4 history = texture2D(uHistoryFrame, vTexCoord);
    vec4 result = mix(current, history, uBlendFactor);
    gl_FragColor = vec4(result.rgb, 1.0);
}
)";

const char* drawFragmentShaderSource = R"(
precision mediump float;
varying vec2 vTexCoord;
uniform sampler2D uTexture;
void main() {
    vec4 color = texture2D(uTexture, vTexCoord);
    gl_FragColor = vec4(color.rgb, 1.0);
}
)";

static bool motion_blur_enabled = false;
static float blur_strength = 0.85f;

static GLuint rawTexture = 0;
static GLuint historyTextures[2] = {0, 0};
static GLuint historyFBOs[2] = {0, 0};
static int pingPongIndex = 0;
static bool isFirstFrame = true;

static GLuint blendShaderProgram = 0;
static GLint blendPosLoc = -1, blendTexCoordLoc = -1, blendCurrentLoc = -1, blendHistoryLoc = -1, blendFactorLoc = -1;
static GLuint drawShaderProgram = 0;
static GLint drawPosLoc = -1, drawTexCoordLoc = -1, drawTextureLoc = -1;
static GLuint vertexBuffer = 0, indexBuffer = 0;
static int blur_res_width = 0, blur_res_height = 0;

void initializeMotionBlurResources(GLint width, GLint height) {
    if (rawTexture != 0) {
        glDeleteTextures(1, &rawTexture); glDeleteTextures(2, historyTextures); glDeleteFramebuffers(2, historyFBOs);
    }
    if (blendShaderProgram == 0) {
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &vertexShaderSource, nullptr); glCompileShader(vs);

        GLuint fsBlend = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fsBlend, 1, &blendFragmentShaderSource, nullptr); glCompileShader(fsBlend);

        blendShaderProgram = glCreateProgram();
        glAttachShader(blendShaderProgram, vs); glAttachShader(blendShaderProgram, fsBlend); glLinkProgram(blendShaderProgram);

        blendPosLoc = glGetAttribLocation(blendShaderProgram, "aPosition");
        blendTexCoordLoc = glGetAttribLocation(blendShaderProgram, "aTexCoord");
        blendCurrentLoc = glGetUniformLocation(blendShaderProgram, "uCurrentFrame");
        blendHistoryLoc = glGetUniformLocation(blendShaderProgram, "uHistoryFrame");
        blendFactorLoc = glGetUniformLocation(blendShaderProgram, "uBlendFactor");

        GLuint fsDraw = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fsDraw, 1, &drawFragmentShaderSource, nullptr); glCompileShader(fsDraw);

        drawShaderProgram = glCreateProgram();
        glAttachShader(drawShaderProgram, vs); glAttachShader(drawShaderProgram, fsDraw); glLinkProgram(drawShaderProgram);

        drawPosLoc = glGetAttribLocation(drawShaderProgram, "aPosition");
        drawTexCoordLoc = glGetAttribLocation(drawShaderProgram, "aTexCoord");
        drawTextureLoc = glGetUniformLocation(drawShaderProgram, "uTexture");

        GLfloat vertices[] = { -1.0f, 1.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f };
        GLushort indices[] = { 0, 1, 2, 0, 2, 3 };

        glGenBuffers(1, &vertexBuffer); glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer); glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
        glGenBuffers(1, &indexBuffer); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer); glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
    }

    glGenTextures(1, &rawTexture); glBindTexture(GL_TEXTURE_2D, rawTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(2, historyTextures); glGenFramebuffers(2, historyFBOs);
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, historyTextures[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, historyFBOs[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, historyTextures[i], 0);
        glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glBindTexture(GL_TEXTURE_2D, 0);
    blur_res_width = width; blur_res_height = height; pingPongIndex = 0; isFirstFrame = true;
}

void apply_motion_blur(int width, int height) {
    if (width != blur_res_width || height != blur_res_height || rawTexture == 0) initializeMotionBlurResources(width, height);

    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, rawTexture);
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, width, height, 0);

    int curr = pingPongIndex, prev = 1 - pingPongIndex;
    if (isFirstFrame) {
        glBindFramebuffer(GL_FRAMEBUFFER, historyFBOs[curr]); glViewport(0, 0, width, height);
        glUseProgram(drawShaderProgram); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, rawTexture); glUniform1i(drawTextureLoc, 0);
        glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
        glEnableVertexAttribArray(drawPosLoc); glVertexAttribPointer(drawPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
        glEnableVertexAttribArray(drawTexCoordLoc); glVertexAttribPointer(drawTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)(2 * sizeof(GLfloat)));
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
        isFirstFrame = false;
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, historyFBOs[curr]); glViewport(0, 0, width, height);
        glUseProgram(blendShaderProgram);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, rawTexture); glUniform1i(blendCurrentLoc, 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, historyTextures[prev]); glUniform1i(blendHistoryLoc, 1);
        glUniform1f(blendFactorLoc, blur_strength);
        glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
        glEnableVertexAttribArray(blendPosLoc); glVertexAttribPointer(blendPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
        glEnableVertexAttribArray(blendTexCoordLoc); glVertexAttribPointer(blendTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)(2 * sizeof(GLfloat)));
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, width, height); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(drawShaderProgram); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, historyTextures[curr]); glUniform1i(drawTextureLoc, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
    glEnableVertexAttribArray(drawPosLoc); glVertexAttribPointer(drawPosLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
    glEnableVertexAttribArray(drawTexCoordLoc); glVertexAttribPointer(drawTexCoordLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)(2 * sizeof(GLfloat)));
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    pingPongIndex = prev;
}

// ==========================================
// [3] 입력 훅 (libinput.so)
// ==========================================
static void (*initMotionEvent)(void*, void*, void*) = nullptr;
static void HookInput1(void* thiz, void* a1, void* a2) {
    if (initMotionEvent) initMotionEvent(thiz, a1, a2);
    if (thiz && g_Initialized) ImGui_ImplAndroid_HandleInputEvent((AInputEvent*)thiz);
}

static int32_t (*Consume)(void*, void*, bool, long, uint32_t*, AInputEvent**) = nullptr;
static int32_t HookInput2(void* thiz, void* a1, bool a2, long a3, uint32_t* a4, AInputEvent** event) {
    int32_t result = Consume ? Consume(thiz, a1, a2, a3, a4, event) : 0;
    if (result == 0 && event && *event && g_Initialized) ImGui_ImplAndroid_HandleInputEvent(*event);
    return result;
}

// ==========================================
// [4] 마인크래프트 메모리 패치 로직
// ==========================================
static uint32_t EncodeCmpW8Imm_Table(int imm) {
    if (imm < 0 || imm > 575) return 0;
    uint32_t instr = 0x7100001F;
    int block = imm / 64, offset = imm % 64;
    uint8_t* p = reinterpret_cast<uint8_t*>(&instr);
    p[1] = 0x01 + (offset * 0x04);
    p[2] = (uint8_t)block;
    return instr;
}

static void ScanSignatures() {
    uintptr_t base = GlossGetLibSection("libminecraftpe.so", ".text", nullptr);
    size_t size = 0;
    while ((base = GlossGetLibSection("libminecraftpe.so", ".text", &size)) == 0 || size == 0) usleep(10000);
    
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

    g_PatchAddrs.assign(signatures.size(), 0);
    g_Originals.assign(signatures.size(), {0, 0, 0, 0});

    for (size_t s = 0; s < signatures.size(); s++) {
        for (size_t i = 0; i + signatures[s].size() < size; i++) {
            if (memcmp((void*)(base + i), signatures[s].data(), signatures[s].size()) == 0) {
                g_PatchAddrs[s] = base + i;
                memcpy(g_Originals[s].data(), (void*)(base + i), 4);
                break;
            }
        }
    }
    g_PatchesReady = true;
}

// ==========================================
// [5] 통합 메뉴 UI
// ==========================================
static void DrawMenu() {
    ImGui::SetNextWindowPos(ImVec2(10, 80), ImGuiCond_FirstUseEver);
    ImGui::Begin("AnarchyArray Menu", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    // 1. Minecraft Patches 섹션
    if (ImGui::CollapsingHeader("Minecraft Patches", ImGuiTreeNodeFlags_DefaultOpen)) {
        static bool infinitySpread = false;
        static bool spongePlus = false;
        static bool spongePlusPlus = false;
        static int absorbTypeVal = 5;

        if (ImGui::Checkbox("InfinitySpread", &infinitySpread) && g_PatchesReady) {
            const uint8_t patch[] = {0x03, 0x00, 0x80, 0x52};
            for (size_t i = 0; i < 4 && i < g_PatchAddrs.size(); i++) {
                if (g_PatchAddrs[i] != 0) WriteMemory((void*)g_PatchAddrs[i], infinitySpread ? (void*)patch : (void*)g_Originals[i].data(), 4, true);
            }
        }

        if (ImGui::Checkbox("SpongeRange+", &spongePlus) && g_PatchesReady) {
            const uint8_t patchPlus[] = {0x1F, 0x20, 0x03, 0xD5, 0xFB, 0x13, 0x40, 0xF9, 0x7F, 0x07, 0x00, 0xB1};
            if (4 < g_PatchAddrs.size() && g_PatchAddrs[4] != 0) {
                WriteMemory((void*)g_PatchAddrs[4], spongePlus ? (void*)patchPlus : (void*)g_Originals[4].data(), spongePlus ? sizeof(patchPlus) : 4, true);
            }
        }

        ImGui::BeginDisabled(!spongePlus);
        if (ImGui::Checkbox("SpongeRange++", &spongePlusPlus) && g_PatchesReady) {
            const uint8_t patchPlusPlus[] = {0x5F, 0xFD, 0x03, 0xF1, 0x8B, 0x2D, 0x0D, 0x9B};
            if (5 < g_PatchAddrs.size() && g_PatchAddrs[5] != 0) {
                WriteMemory((void*)g_PatchAddrs[5], spongePlusPlus ? (void*)patchPlusPlus : (void*)g_Originals[5].data(), spongePlusPlus ? sizeof(patchPlusPlus) : 4, true);
            }
        }
        ImGui::EndDisabled();

        ImGui::Text("Absorb Type:"); ImGui::SameLine();
        ImGui::SetNextItemWidth(50);
        ImGui::InputInt("##absorbDisplay", &absorbTypeVal, 0, 0, ImGuiInputTextFlags_ReadOnly); ImGui::SameLine();
        
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 6)); ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
        if (ImGui::Button("K", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) ImGui::OpenPopup("AbsorbKeypad"); ImGui::SameLine();
        if (ImGui::Button("i", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) ImGui::OpenPopup("AbsorbTypeInfo"); ImGui::SameLine();
        if (ImGui::Button("-", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())) && absorbTypeVal > 0) absorbTypeVal--; ImGui::SameLine();
        if (ImGui::Button("+", ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())) && absorbTypeVal < 575) absorbTypeVal++;
        ImGui::PopStyleVar(2);

        if (g_PatchesReady && absorbTypeVal >= 0 && absorbTypeVal <= 575) {
            for (size_t idx : {6, 7}) {
                if (idx < g_PatchAddrs.size() && g_PatchAddrs[idx] != 0) {
                    uint32_t instr = EncodeCmpW8Imm_Table(absorbTypeVal);
                    if (instr != 0) WriteMemory((void*)g_PatchAddrs[idx], &instr, 4, true);
                }
            }
        }

        if (ImGui::BeginPopup("AbsorbTypeInfo", ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("0=Air, 1=Dirt, 2=Wood, 5=Water, 6=Lava, 12=TNT...");
            if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        if (ImGui::BeginPopup("AbsorbKeypad", ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Keypad");
            const float cw = 60.0f, rh = 50.0f;
            for (int i=1; i<=9; i++) {
                if (ImGui::Button(std::to_string(i).c_str(), ImVec2(cw, rh))) absorbTypeVal = absorbTypeVal * 10 + i;
                if (i % 3 != 0) ImGui::SameLine();
            }
            ImGui::Dummy(ImVec2(cw, rh)); ImGui::SameLine();
            if (ImGui::Button("0", ImVec2(cw, rh))) absorbTypeVal = absorbTypeVal * 10; ImGui::SameLine();
            if (ImGui::Button("<-", ImVec2(cw, rh))) absorbTypeVal /= 10;
            if (ImGui::Button("Close", ImVec2(cw*3+8, rh/2))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    // 2. Motion Blur 섹션
    if (ImGui::CollapsingHeader("Visual Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enable Motion Blur", &motion_blur_enabled);
        if (motion_blur_enabled) {
            ImGui::Text("Blur Strength");
            ImGui::SliderFloat("##Strength", &blur_strength, 0.0f, 0.98f, "%.2f");
        }
    }

    ImGui::End();
}

// ==========================================
// [6] ImGui 초기화 및 메인 렌더링 루프
// ==========================================
static void Setup() {
    if (g_Initialized || !g_Window) return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
    
    float scale = (float)g_Height / 720.0f;
    scale = std::max(1.5f, std::min(scale, 4.0f));
    
    ImFontConfig cfg; cfg.SizePixels = 18.0f * scale;
    io.Fonts->AddFontDefault(&cfg);
    
    ImGui_ImplAndroid_Init(g_Window);
    ImGui_ImplOpenGL3_Init("#version 300 es");
    ImGui::GetStyle().ScaleAllSizes(scale * 0.65f);
    
    g_Initialized = true;
    LOGI("ImGui Initialized!");
}

static void Render() {
    if (!g_Initialized) return;

    // OpenGL 상태 철저히 저장
    GLint last_prog; glGetIntegerv(GL_CURRENT_PROGRAM, &last_prog);
    GLint last_tex; glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_tex);
    GLint last_active_tex; glGetIntegerv(GL_ACTIVE_TEXTURE, &last_active_tex);
    GLint last_array_buffer; glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);
    GLint last_element_array_buffer; glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_element_array_buffer);
    GLint last_vao; glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vao);
    GLint last_fbo; glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_fbo);
    GLint last_viewport[4]; glGetIntegerv(GL_VIEWPORT, last_viewport);
    GLint last_unpack; glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &last_unpack);
    
    GLboolean last_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean last_depth = glIsEnabled(GL_DEPTH_TEST);
    GLboolean last_blend = glIsEnabled(GL_BLEND);
    GLboolean last_cull = glIsEnabled(GL_CULL_FACE);

    // 1. 모션 블러 효과 적용
    if (motion_blur_enabled) apply_motion_blur(g_Width, g_Height);

    // 2. ImGui 렌더링을 위한 안전한 상태 설정 (투명화/충돌 방지 핵심)
    glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindVertexArray(0);

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)g_Width, (float)g_Height);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame(); 
    ImGui::NewFrame();
    
    DrawMenu();
    
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // 3. 게임 원래 상태로 완벽 복구
    glUseProgram(last_prog);
    glActiveTexture(last_active_tex);
    glBindTexture(GL_TEXTURE_2D, last_tex);
    glBindVertexArray(last_vao);
    glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, last_element_array_buffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, last_unpack);
    glBindFramebuffer(GL_FRAMEBUFFER, last_fbo);
    glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
    
    if (last_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (last_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (last_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (last_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
}

// ==========================================
// [7] EGL 및 메인 훅 세팅
// ==========================================
static EGLSurface hook_eglCreateWindowSurface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint* attrib_list) {
    if (win) g_Window = (ANativeWindow*)win;
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
        eglQuerySurface(dpy, surf, EGL_WIDTH, &g_Width);
        eglQuerySurface(dpy, surf, EGL_HEIGHT, &g_Height);
        
        if (!g_Initialized && g_Window && g_Width > 0 && g_Height > 0) Setup();
        if (g_Initialized) Render();
    }
    return orig_eglSwapBuffers(dpy, surf);
}

static void HookInput() {
    void* sym1 = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer21initializeMotionEventEPNS_11MotionEventEPKNS_12InputMessageE", nullptr);
    if (sym1) GlossHook(sym1, (void*)HookInput1, (void**)&initMotionEvent);

    void* sym2 = (void*)GlossSymbol(GlossOpen("libinput.so"), "_ZN7android13InputConsumer7consumeEPNS_26InputEventFactoryInterfaceEblPjPPNS_10InputEventE", nullptr);
    if (sym2) GlossHook(sym2, (void*)HookInput2, (void**)&Consume);
}

static void* MainThread(void*) {
    sleep(3); // 게임 및 라이브러리 로드 대기
    GlossInit(true);
    
    GHandle hEGL = GlossOpen("libEGL.so");
    if (hEGL) {
        void* swap = (void*)GlossSymbol(hEGL, "eglSwapBuffers", nullptr);
        if (swap) GlossHook(swap, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
        void* create = (void*)GlossSymbol(hEGL, "eglCreateWindowSurface", nullptr);
        if (create) GlossHook(create, (void*)hook_eglCreateWindowSurface, (void**)&orig_eglCreateWindowSurface);
        void* makeCurrent = (void*)GlossSymbol(hEGL, "eglMakeCurrent", nullptr);
        if (makeCurrent) GlossHook(makeCurrent, (void*)hook_eglMakeCurrent, (void**)&orig_eglMakeCurrent);
    }
    
    GHandle hAndroid = GlossOpen("libandroid.so");
    if (hAndroid) {
        void* f = (void*)GlossSymbol(hAndroid, "ANativeWindow_fromSurface", nullptr);
        if (f) GlossHook(f, (void*)hook_ANativeWindow_fromSurface, (void**)&orig_ANativeWindow_fromSurface);
    }
    
    HookInput();
    ScanSignatures();
    LOGI("MainThread finished setup");
    return nullptr;
}

__attribute__((constructor))
void AnarchyArray_Init() {
    pthread_t t;
    pthread_create(&t, nullptr, MainThread, nullptr);
}
