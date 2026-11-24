// HapWallpaper - fondo de pantalla animado ultraliviano para Windows 11.
// Reproduce un video en bucle detras de los iconos del escritorio, en todos los
// monitores. Con un archivo HAP (cuadros BC1) no usa el decodificador de video: la
// placa lee los cuadros tal cual. Sin HAP usa Media Foundation (decodificacion por
// hardware). Presenta en cada refresco del monitor para que G-Sync/FreeSync no baje la
// frecuencia, y se pausa por monitor con ventanas maximizadas o a pantalla completa,
// con la sesion bloqueada o con la pantalla apagada.
//
// Uso: HapWallpaper.exe [ruta_video|ruta.hap] [--debug] [--nopause] [--notaskbar] [--forcetaskbar] [--noautostart]
//   La barra transparente viene prendida y se puede apagar desde el menu de la bandeja
//   (se guarda en HapWallpaper.ini).
//   Si TranslucentTB o un mod de barra de Windhawk ya manejan la barra, no la toca
//   (solo un programa puede usar el diagnostico XAML de Explorer); --forcetaskbar la
//   activa ignorando eso, --notaskbar la apaga en esta sesion.
//   Sin ruta usa "wallpaper.hap" junto al .exe si existe y si no "wallpaper.mp4"
//   (tambien acepta los nombres anteriores fondo.hap / fondo.mp4).
//   --debug escribe log.txt junto al .exe (y tb_log.txt la DLL de la barra).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cwchar>

// GUIDs (definidos aqui para no depender de que mfuuid los traiga).
DEFINE_GUID(FV_CLSID_MFMediaEngineClassFactory, 0xb44392da, 0x499b, 0x446b, 0xa4, 0xcb, 0x00, 0x5f, 0xea, 0xd0, 0xe6, 0xd5);
DEFINE_GUID(FV_IID_IMFMediaEngineClassFactory, 0x4d645ace, 0x26aa, 0x4688, 0x9b, 0xe1, 0xdf, 0x35, 0x16, 0x99, 0x0b, 0x93);
DEFINE_GUID(FV_IID_IMFMediaEngineNotify, 0xfee7c112, 0xe776, 0x42b5, 0x9b, 0xbf, 0x00, 0x48, 0x52, 0x4e, 0x2b, 0xd5);
DEFINE_GUID(FV_MF_MEDIA_ENGINE_CALLBACK, 0xc60381b8, 0x83a4, 0x41f8, 0xa3, 0xd0, 0xde, 0x05, 0x07, 0x68, 0x49, 0xa9);
DEFINE_GUID(FV_MF_MEDIA_ENGINE_PLAYBACK_HWND, 0xd988879b, 0x67c9, 0x4d92, 0xba, 0xa7, 0x6e, 0xad, 0xd4, 0x46, 0x03, 0x9d);
DEFINE_GUID(FV_MF_MEDIA_ENGINE_DXGI_MANAGER, 0x065702da, 0x1094, 0x486d, 0x86, 0x17, 0xee, 0x7c, 0xc4, 0xee, 0x46, 0x48);
DEFINE_GUID(FV_MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, 0x5066893c, 0x8cf9, 0x42bc, 0x8b, 0x8a, 0x47, 0x22, 0x12, 0xe5, 0x27, 0x26);
DEFINE_GUID(FV_IID_ID3D10Multithread, 0x9b7e4e00, 0x342c, 0x4106, 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0);
DEFINE_GUID(FV_GUID_CONSOLE_DISPLAY_STATE, 0x6fe69556, 0x704a, 0x47a0, 0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47);

#define WM_TRAY (WM_APP + 1)
#define IDT_CHECK 1
#define IDM_TOGGLE 100
#define IDM_EXIT 101
#define IDM_TASKBAR 102

static HINSTANCE g_inst;
static HWND g_msgWnd;
static ID3D11Device* g_d3d;
static IMFDXGIDeviceManager* g_dxgiMgr;
static UINT g_resetToken;
static CRITICAL_SECTION g_ctxLock; // el contexto inmediato se comparte entre los hilos de cada monitor
static std::wstring g_videoPath, g_exeDir;
static bool g_debug, g_noAutostart, g_noAutoPause, g_userPaused, g_fullscreenApp, g_locked, g_displayOff;
static UINT WM_TASKBARCREATED;
static NOTIFYICONDATAW g_nid;
static const wchar_t* g_tbOwner; // otro programa que maneja la barra (TranslucentTB, Windhawk)

static void Log(const wchar_t* fmt, ...) {
    if (!g_debug) return;
    FILE* f = _wfopen((g_exeDir + L"\\log.txt").c_str(), L"a, ccs=UTF-8");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fwprintf(f, L"%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list ap; va_start(ap, fmt); vfwprintf(f, fmt, ap); va_end(ap);
    fputwc(L'\n', f);
    fclose(f);
}

static void Balloon(const wchar_t* title, const wchar_t* text) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    wcsncpy(n.szInfoTitle, title, 63);
    wcsncpy(n.szInfo, text, 255);
    n.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// ------------------------------------------------------------------ Un monitor
// Cada monitor tiene su ventana dentro de WorkerW, su swap chain y su hilo de render.
// El hilo presenta en cada refresco del monitor: G-Sync se queda a la frecuencia
// maxima aunque el video sea de 30 fps. En los refrescos en los que el cuadro no cambio
// no dibuja nada y le pasa a Windows un rectangulo sucio minimo.
struct Screen {
    HMONITOR mon = nullptr;
    RECT full = {}, work = {};
    HWND wnd = nullptr;
    IMFMediaEngine* engine = nullptr;      // solo en modo video
    bool engineOwnsWindow = false;          // sin D3D11: el motor dibuja en la ventana
    IDXGISwapChain1* swap = nullptr;
    HANDLE frameWait = nullptr, runEvent = nullptr, thread = nullptr;
    volatile LONG stop = 0;
    volatile bool shown = false;            // ya mostro al menos un cuadro
    bool autoPaused = false;
    ID3D11Texture2D* hapTex = nullptr;      // modo HAP: cuadro BC1 de este monitor
    ID3D11ShaderResourceView* hapSrv = nullptr;
    ID3D11Buffer* cb = nullptr;             // recorte para llenar la pantalla
};
static std::vector<Screen*> g_screens;

// Rectangulo del video (0..1) que llena la pantalla sin bordes, recortando al centro.
static void CoverRect(float vw, float vh, float sw, float sh, float out[4] /*x0,y0,x1,y1*/) {
    float va = vw / vh, sa = sw / sh;
    out[0] = 0; out[1] = 0; out[2] = 1; out[3] = 1;
    if (va > sa) { float s = sa / va; out[0] = (1 - s) / 2; out[2] = out[0] + s; }
    else if (va < sa) { float s = va / sa; out[1] = (1 - s) / 2; out[3] = out[1] + s; }
}

// ---------------------------------------------------------------- Media Engine
class Notify : public IMFMediaEngineNotify {
    LONG m_ref = 1;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == FV_IID_IMFMediaEngineNotify) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }
    STDMETHODIMP EventNotify(DWORD ev, DWORD_PTR p1, DWORD p2) override {
        if (ev == MF_MEDIA_ENGINE_EVENT_ERROR) Log(L"Media Engine error %u hr=0x%08X", (unsigned)p1, (unsigned)p2);
        return S_OK;
    }
};

// Media Foundation se carga solo si hace falta reproducir video (en modo HAP no se usa).
static bool g_mfStarted;
static void EnsureMF() {
    if (!g_mfStarted) { MFStartup(MF_VERSION, MFSTARTUP_FULL); g_mfStarted = true; }
}

// video = true: dispositivo con soporte de video y administrador DXGI para Media Foundation.
// En modo HAP alcanza con un dispositivo comun (menos memoria del driver).
static void InitD3D(bool video) {
    if (g_d3d) return;
    D3D_FEATURE_LEVEL fl;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (video ? D3D11_CREATE_DEVICE_VIDEO_SUPPORT : 0);
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                 D3D11_SDK_VERSION, &g_d3d, &fl, nullptr))) {
        Log(L"D3D11 sin aceleracion de video; se usa modo por defecto");
        g_d3d = nullptr; return;
    }
    ID3D10Multithread* mt = nullptr;
    if (SUCCEEDED(g_d3d->QueryInterface(FV_IID_ID3D10Multithread, (void**)&mt))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
    if (video) {
        EnsureMF();
        if (SUCCEEDED(MFCreateDXGIDeviceManager(&g_resetToken, &g_dxgiMgr)))
            g_dxgiMgr->ResetDevice(g_d3d, g_resetToken);
    }
}

static IMFMediaEngine* CreateEngine(HWND hwnd, bool useDxgi) {
    EnsureMF();
    IMFMediaEngineClassFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(FV_CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  FV_IID_IMFMediaEngineClassFactory, (void**)&factory);
    if (FAILED(hr)) { Log(L"No se pudo crear la fabrica: 0x%08X", (unsigned)hr); return nullptr; }
    IMFAttributes* attr = nullptr;
    MFCreateAttributes(&attr, 4);
    Notify* notify = new Notify();
    attr->SetUnknown(FV_MF_MEDIA_ENGINE_CALLBACK, notify);
    // Con DXGI: modo frame server (sin ventana); sin DXGI: el motor dibuja en la ventana.
    if (useDxgi && g_dxgiMgr) attr->SetUnknown(FV_MF_MEDIA_ENGINE_DXGI_MANAGER, g_dxgiMgr);
    else attr->SetUINT64(FV_MF_MEDIA_ENGINE_PLAYBACK_HWND, (UINT64)(ULONG_PTR)hwnd);
    attr->SetUINT32(FV_MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, 87 /* DXGI_FORMAT_B8G8R8A8_UNORM */);
    IMFMediaEngine* engine = nullptr;
    hr = factory->CreateInstance(0, attr, &engine);
    notify->Release(); attr->Release(); factory->Release();
    if (FAILED(hr)) { Log(L"CreateInstance fallo (dxgi=%d): 0x%08X", useDxgi, (unsigned)hr); return nullptr; }
    engine->SetMuted(TRUE);
    engine->SetLoop(TRUE);
    engine->SetAutoPlay(TRUE);
    BSTR src = SysAllocString(g_videoPath.c_str());
    hr = engine->SetSource(src);
    SysFreeString(src);
    if (FAILED(hr)) Log(L"SetSource fallo: 0x%08X", (unsigned)hr);
    engine->Play();
    return engine;
}

// ------------------------------------------------- Modo HAP (cuadros ya comprimidos)
// wallpaper.hap: cuadros DXT1/BC1 de HAP sin compresion extra (ffmpeg -c:v hap -compressor
// none -f rawvideo). La placa lee BC1 directamente: no se usa el decodificador de video,
// solo se sube el cuadro y se dibuja con un shader que lo escala para llenar cada monitor.
// Al final del archivo, prepare_video.ps1 agrega un bloque de 32 bytes con los datos
// del video. Sin el bloque se asume 30 fps y la resolucion del monitor principal.
#pragma pack(push, 1)
struct HapTrailer { char magic[8]; UINT width, height, fpsNum, fpsDen, frames, stride; };
#pragma pack(pop)
static const char kHapMagic[8] = { 'H', 'A', 'P', 'W', 'A', 'L', 'L', '1' };
static std::wstring g_hapPath;
static bool g_hapChecked, g_hapReady; // archivo validado y shaders listos
static UINT g_hapFrames, g_hapStride, g_hapHdr, g_hapPayload, g_hapW, g_hapH, g_hapFpsNum = 30, g_hapFpsDen = 1;
static ID3D11VertexShader* g_vs;
static ID3D11PixelShader* g_ps;
static ID3D11SamplerState* g_sampler;

typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static bool ReadAt(HANDLE f, LONGLONG off, void* dst, DWORD len) {
    LARGE_INTEGER li; li.QuadPart = off;
    DWORD n = 0;
    return SetFilePointerEx(f, li, nullptr, FILE_BEGIN) && ReadFile(f, dst, len, &n, nullptr) && n == len;
}

// Valida el archivo leyendo solo el primer encabezado y el bloque final (no lo mapea).
static bool ValidateHap() {
    HANDLE f = CreateFileW(g_hapPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size; GetFileSizeEx(f, &size);
    BYTE p[8] = {};
    HapTrailer t = {};
    bool ok = size.QuadPart > 40 && ReadAt(f, 0, p, 8) && ReadAt(f, size.QuadPart - sizeof(t), &t, sizeof(t));
    CloseHandle(f);
    ok = ok && (p[3] & 0x0F) == 0x0B && (p[3] & 0xF0) == 0xA0; // DXT1, sin Snappy
    if (!ok) return false;
    UINT s24 = p[0] | (p[1] << 8) | (p[2] << 16);
    g_hapHdr = s24 ? 4 : 8;
    g_hapPayload = s24 ? s24 : *(const UINT*)(p + 4);
    g_hapStride = g_hapHdr + g_hapPayload;
    LONGLONG data = size.QuadPart;
    if (!memcmp(t.magic, kHapMagic, 8)) {
        data -= sizeof(t);
        g_hapW = t.width; g_hapH = t.height;
        g_hapFpsNum = t.fpsNum; g_hapFpsDen = t.fpsDen;
        ok = t.fpsNum && t.fpsDen && t.stride == g_hapStride && (LONGLONG)t.frames * t.stride == data;
    } else {
        // Archivo sin bloque final: se asume la resolucion del monitor principal.
        g_hapW = GetSystemMetrics(SM_CXSCREEN); g_hapH = GetSystemMetrics(SM_CYSCREEN);
        g_hapFpsNum = 30; g_hapFpsDen = 1;
    }
    ok = ok && g_hapW % 4 == 0 && g_hapH % 4 == 0 && g_hapPayload == g_hapW * g_hapH / 2 && data % g_hapStride == 0;
    g_hapFrames = ok ? (UINT)(data / g_hapStride) : 0;
    return ok && g_hapFrames;
}

static void CloseHap() { g_hapReady = false; }

static bool InitHap() {
    if (g_hapReady) return true;
    if (g_hapPath.empty() || !g_d3d || !g_hapFrames) return false;
    HRESULT hr = S_OK;
    if (!g_vs) {
        // El triangulo cubre la pantalla; uv recorta el video para llenarla (cb).
        static const char hlsl[] =
            "Texture2D t : register(t0); SamplerState s : register(s0);\n"
            "cbuffer c : register(b0) { float4 r; };\n"
            "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD; };\n"
            "V vs(uint i : SV_VertexID) {\n"
            "  float2 p = float2((i << 1) & 2, i & 2); V o;\n"
            "  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
            "  o.uv = r.xy + p * (r.zw - r.xy); return o; }\n"
            "float4 ps(V i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1); }\n";
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        PFN_D3DCompile compile = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
        ID3DBlob *vsb = nullptr, *psb = nullptr;
        hr = compile ? S_OK : E_FAIL;
        if (SUCCEEDED(hr)) hr = compile(hlsl, sizeof(hlsl) - 1, nullptr, nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, nullptr);
        if (SUCCEEDED(hr)) hr = compile(hlsl, sizeof(hlsl) - 1, nullptr, nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, nullptr);
        if (SUCCEEDED(hr)) hr = g_d3d->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
        if (SUCCEEDED(hr)) hr = g_d3d->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
        if (vsb) vsb->Release();
        if (psb) psb->Release();
        if (SUCCEEDED(hr)) {
            D3D11_SAMPLER_DESC sd = {};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            hr = g_d3d->CreateSamplerState(&sd, &g_sampler);
        }
    }
    if (FAILED(hr)) { Log(L"No se pudo preparar HAP: 0x%08X", (unsigned)hr); CloseHap(); return false; }
    g_hapReady = true;
    Log(L"HAP: %u cuadros %ux%u a %u/%u fps", g_hapFrames, g_hapW, g_hapH, g_hapFpsNum, g_hapFpsDen);
    return true;
}

static bool CreateHapResources(Screen* s, UINT sw, UINT sh) {
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = g_hapW; td.Height = g_hapH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_BC1_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_d3d->CreateTexture2D(&td, nullptr, &s->hapTex))) return false;
    if (FAILED(g_d3d->CreateShaderResourceView(s->hapTex, nullptr, &s->hapSrv))) return false;
    float r[4]; CoverRect((float)g_hapW, (float)g_hapH, (float)sw, (float)sh, r);
    D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA init = { r, 0, 0 };
    return SUCCEEDED(g_d3d->CreateBuffer(&bd, &init, &s->cb));
}

// --------------------------------------------------------------------- Render
static bool ShouldPause(Screen* s) {
    bool want = g_userPaused || s->autoPaused || g_fullscreenApp || g_locked || g_displayOff;
    // No pausar hasta haber mostrado un cuadro (si no, detras de la barra queda negro).
    return want && (s->shown || !s->swap);
}

static DWORD WINAPI RenderThread(LPVOID param) {
    Screen* s = (Screen*)param;
    ID3D11DeviceContext* ctx = nullptr;
    g_d3d->GetImmediateContext(&ctx);
    bool hap = g_hapReady && s->hapTex;
    bool waitNext = false, haveFrame = false;
    UINT64 frameVer = 0, bufVer[4] = { ~0ull, ~0ull, ~0ull, ~0ull };
    IDXGISwapChain3* sc3 = nullptr;
    s->swap->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3);
    DXGI_SWAP_CHAIN_DESC1 sd; s->swap->GetDesc1(&sd);
    LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);
    LONGLONG hapLast = 0, hapAcc = 0;
    UINT hapCur = ~0u;
    // Lectura de cuadros: el archivo HAP no se carga en memoria. Se lee del disco solo el
    // cuadro que toca (~1,8 MB a 1440p), sin la cache de Windows (FILE_FLAG_NO_BUFFERING),
    // en dos buffers: mientras se muestra uno, el siguiente cuadro ya se esta leyendo.
    const DWORD kSector = 4096;
    HANDLE hf = INVALID_HANDLE_VALUE;
    BYTE* buf[2] = {};
    OVERLAPPED ov[2] = {};
    bool pending[2] = {};
    UINT bufFrame[2] = { ~0u, ~0u }, delta[2] = {};
    DWORD bufSize = (DWORD)(((ULONGLONG)g_hapPayload + 2 * kSector + kSector - 1) & ~(ULONGLONG)(kSector - 1));
    auto finish = [&](int b) -> bool {
        if (!pending[b]) return false;
        pending[b] = false;
        DWORD n = 0;
        return GetOverlappedResult(hf, &ov[b], &n, TRUE) && n >= delta[b] + g_hapPayload;
    };
    auto issue = [&](int b, UINT f) {
        ULONGLONG off = (ULONGLONG)f * g_hapStride + g_hapHdr;
        ULONGLONG start = off & ~(ULONGLONG)(kSector - 1);
        DWORD len = (DWORD)((off + g_hapPayload - start + kSector - 1) & ~(ULONGLONG)(kSector - 1));
        ov[b].Offset = (DWORD)start; ov[b].OffsetHigh = (DWORD)(start >> 32);
        ResetEvent(ov[b].hEvent);
        delta[b] = (UINT)(off - start); bufFrame[b] = f;
        pending[b] = ReadFile(hf, buf[b], len, nullptr, &ov[b]) || GetLastError() == ERROR_IO_PENDING;
        if (!pending[b]) bufFrame[b] = ~0u;
    };
    auto releaseHap = [&]() {
        for (int b = 0; b < 2; b++) {
            if (pending[b]) finish(b);
            if (buf[b]) { VirtualFree(buf[b], 0, MEM_RELEASE); buf[b] = nullptr; }
            if (ov[b].hEvent) { CloseHandle(ov[b].hEvent); ov[b].hEvent = nullptr; }
            bufFrame[b] = ~0u;
        }
        if (hf != INVALID_HANDLE_VALUE) { CloseHandle(hf); hf = INVALID_HANDLE_VALUE; }
    };
    MFVideoNormalizedRect src = { 0, 0, 1, 1 };
    bool srcReady = false;
    while (!s->stop) {
        if (WaitForSingleObject(s->runEvent, 0) == WAIT_TIMEOUT) {
            // En pausa: soltar el archivo y los buffers hasta que vuelva a reproducirse.
            if (hf != INVALID_HANDLE_VALUE) {
                releaseHap();
                SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            }
            hapLast = 0;
            WaitForSingleObject(s->runEvent, INFINITE);
        }
        if (s->stop) break;
        if (hap && hf == INVALID_HANDLE_VALUE) {
            hf = CreateFileW(g_hapPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            for (int b = 0; b < 2 && hf != INVALID_HANDLE_VALUE; b++) {
                buf[b] = (BYTE*)VirtualAlloc(nullptr, bufSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                ov[b].hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            }
            if (hf == INVALID_HANDLE_VALUE || !buf[0] || !buf[1]) { releaseHap(); Sleep(500); continue; }
        }
        // Espera el proximo refresco (la cola del swap chain tiene lugar).
        if (waitNext && s->frameWait && WaitForSingleObject(s->frameWait, 100) != WAIT_OBJECT_0) { waitNext = false; continue; }
        waitNext = false;
        if (hap) {
            // Reloj propio: solo avanza mientras se reproduce.
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            if (hapLast) { LONGLONG dt = now.QuadPart - hapLast; if (dt > qpf.QuadPart / 20) dt = qpf.QuadPart / 20; hapAcc += dt; }
            hapLast = now.QuadPart;
            UINT f = (UINT)((hapAcc * (LONGLONG)g_hapFpsNum / (qpf.QuadPart * (LONGLONG)g_hapFpsDen)) % g_hapFrames);
            if (f != hapCur) {
                int b = bufFrame[0] == f ? 0 : bufFrame[1] == f ? 1 : -1;
                if (b < 0) { // no estaba adelantado (inicio o despues de una pausa)
                    b = 0;
                    if (pending[1]) finish(1);
                    if (pending[0]) finish(0);
                    issue(0, f);
                }
                if (finish(b)) {
                    EnterCriticalSection(&g_ctxLock);
                    ctx->UpdateSubresource(s->hapTex, 0, nullptr, buf[b] + delta[b], (g_hapW / 4) * 8, 0);
                    LeaveCriticalSection(&g_ctxLock);
                    hapCur = f; frameVer++; haveFrame = true;
                }
                bufFrame[b] = ~0u;
                // Adelantar la lectura del cuadro siguiente en el otro buffer.
                int nb = b ^ 1;
                if (pending[nb]) finish(nb);
                issue(nb, (f + 1) % g_hapFrames);
            }
        } else {
            if (!srcReady) {
                DWORD vw = 0, vh = 0;
                if (FAILED(s->engine->GetNativeVideoSize(&vw, &vh)) || !vw || !vh) { Sleep(10); continue; }
                float r[4]; CoverRect((float)vw, (float)vh, (float)sd.Width, (float)sd.Height, r);
                src = { r[0], r[1], r[2], r[3] };
                srcReady = true;
                Log(L"Video %ux%u en monitor %ux%u", (unsigned)vw, (unsigned)vh, sd.Width, sd.Height);
            }
            LONGLONG pts;
            if (s->engine->OnVideoStreamTick(&pts) == S_OK) { haveFrame = true; frameVer++; }
        }
        if (!haveFrame) { Sleep(2); continue; }
        // Dibuja el cuadro actual en el buffer del swap chain que toca.
        auto drawCurrent = [&]() -> bool {
            bool ok = false;
            ID3D11Texture2D* back = nullptr;
            if (FAILED(s->swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
            if (hap) {
                ID3D11RenderTargetView* rtv = nullptr;
                if (SUCCEEDED(g_d3d->CreateRenderTargetView(back, nullptr, &rtv))) {
                    D3D11_VIEWPORT vp = { 0, 0, (float)sd.Width, (float)sd.Height, 0, 1 };
                    EnterCriticalSection(&g_ctxLock);
                    ctx->OMSetRenderTargets(1, &rtv, nullptr);
                    ctx->RSSetViewports(1, &vp);
                    ctx->IASetInputLayout(nullptr);
                    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                    ctx->VSSetShader(g_vs, nullptr, 0);
                    ctx->VSSetConstantBuffers(0, 1, &s->cb);
                    ctx->PSSetShader(g_ps, nullptr, 0);
                    ctx->PSSetShaderResources(0, 1, &s->hapSrv);
                    ctx->PSSetSamplers(0, 1, &g_sampler);
                    ctx->Draw(3, 0);
                    ID3D11RenderTargetView* none = nullptr;
                    ctx->OMSetRenderTargets(1, &none, nullptr);
                    LeaveCriticalSection(&g_ctxLock);
                    rtv->Release();
                    ok = true;
                }
            } else {
                RECT dst = { 0, 0, (LONG)sd.Width, (LONG)sd.Height };
                EnterCriticalSection(&g_ctxLock);
                ok = SUCCEEDED(s->engine->TransferVideoFrame(back, &src, &dst, nullptr));
                LeaveCriticalSection(&g_ctxLock);
            }
            back->Release();
            return ok;
        };
        // Cada buffer del swap chain se redibuja solo cuando tiene un cuadro viejo (una vez
        // por buffer por cuadro de video, sin copias intermedias).
        UINT idx = sc3 ? sc3->GetCurrentBackBufferIndex() : 0;
        if (idx > 3) idx = 0;
        bool drawn = false;
        if ((!sc3 || bufVer[idx] != frameVer) && drawCurrent()) { bufVer[idx] = frameVer; drawn = true; }
        // En los refrescos en los que el cuadro no cambio se presenta con DO_NOT_SEQUENCE:
        // se vuelve a mostrar el mismo buffer sin copiarlo ni recomponerlo, y el monitor
        // sigue a su frecuencia maxima. (Un rectangulo sucio de 1x1 hacia que DXGI copiara
        // el cuadro entero en cada refresco: 10 % de 3D contra ~2 %.)
        DXGI_PRESENT_PARAMETERS pp = {};
        UINT flags = (!drawn && sc3) ? DXGI_PRESENT_DO_NOT_SEQUENCE : 0;
        if (SUCCEEDED(s->swap->Present1(1, flags, &pp))) { waitNext = true; s->shown = true; }
        else Sleep(5);
    }
    releaseHap();
    if (sc3) sc3->Release();
    ctx->Release();
    return 0;
}

static bool StartRender(Screen* s) {
    RECT rc; GetClientRect(s->wnd, &rc);
    IDXGIDevice* dxdev = nullptr; IDXGIAdapter* adapter = nullptr; IDXGIFactory2* factory = nullptr;
    g_d3d->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev);
    if (dxdev) dxdev->GetAdapter(&adapter);
    if (adapter) adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = rc.right; sd.Height = rc.bottom;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; // conserva el contenido de cada buffer
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    HRESULT hr = factory ? factory->CreateSwapChainForHwnd(g_d3d, s->wnd, &sd, nullptr, nullptr, &s->swap) : E_FAIL;
    if (factory) factory->MakeWindowAssociation(s->wnd, DXGI_MWA_NO_ALT_ENTER);
    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxdev) dxdev->Release();
    if (FAILED(hr)) { Log(L"No se pudo crear el swap chain: 0x%08X", (unsigned)hr); s->swap = nullptr; return false; }
    IDXGISwapChain2* sc2 = nullptr;
    if (SUCCEEDED(s->swap->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2))) {
        sc2->SetMaximumFrameLatency(1);
        s->frameWait = sc2->GetFrameLatencyWaitableObject();
        sc2->Release();
    }
    s->runEvent = CreateEventW(nullptr, TRUE, ShouldPause(s) ? FALSE : TRUE, nullptr);
    s->stop = 0;
    s->thread = CreateThread(nullptr, 0, RenderThread, s, 0, nullptr);
    SetThreadPriority(s->thread, THREAD_PRIORITY_ABOVE_NORMAL);
    return true;
}

static void DestroyScreen(Screen* s) {
    if (s->thread) {
        InterlockedExchange(&s->stop, 1);
        SetEvent(s->runEvent);
        WaitForSingleObject(s->thread, 2000);
        CloseHandle(s->thread);
    }
    if (s->runEvent) CloseHandle(s->runEvent);
    if (s->frameWait) CloseHandle(s->frameWait);
    if (s->swap) s->swap->Release();
    if (s->hapSrv) s->hapSrv->Release();
    if (s->hapTex) s->hapTex->Release();
    if (s->cb) s->cb->Release();
    if (s->engine) { s->engine->Shutdown(); s->engine->Release(); }
    if (s->wnd && IsWindow(s->wnd)) { SetWindowLongPtrW(s->wnd, GWLP_USERDATA, 0); DestroyWindow(s->wnd); }
    delete s;
}

static void DestroyScreens() {
    for (Screen* s : g_screens) DestroyScreen(s);
    g_screens.clear();
}

// ---------------------------------------------------------- Escritorio (WorkerW)
static BOOL CALLBACK FindWorkerProc(HWND top, LPARAM lp) {
    if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr))
        *(HWND*)lp = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
    return TRUE;
}

static HWND FindWorkerW() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) return nullptr;
    DWORD_PTR r;
    // Pide a Explorer que cree la capa WorkerW detras de los iconos.
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &r);
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &r);
    HWND worker = nullptr;
    EnumWindows(FindWorkerProc, (LPARAM)&worker);
    if (!worker) worker = FindWindowExW(progman, nullptr, L"WorkerW", nullptr); // Win11 24H2+
    return worker;
}

static bool g_hapWarned;

static void AttachScreen(HWND worker, HMONITOR mon) {
    Screen* s = new Screen();
    s->mon = mon;
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    s->full = mi.rcMonitor; s->work = mi.rcWork;
    POINT pts[2] = { { mi.rcMonitor.left, mi.rcMonitor.top }, { mi.rcMonitor.right, mi.rcMonitor.bottom } };
    MapWindowPoints(HWND_DESKTOP, worker, pts, 2);
    s->wnd = CreateWindowExW(WS_EX_NOACTIVATE, L"HapWallpaperVideo", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                             pts[0].x, pts[0].y, pts[1].x - pts[0].x, pts[1].y - pts[0].y,
                             worker, nullptr, g_inst, nullptr);
    if (!s->wnd) { Log(L"No se pudo crear la ventana de video"); delete s; return; }
    SetWindowLongPtrW(s->wnd, GWLP_USERDATA, (LONG_PTR)s);
    g_screens.push_back(s);
    RECT rc; GetClientRect(s->wnd, &rc);

    bool ok = false;
    if (g_hapReady) {
        ok = CreateHapResources(s, rc.right, rc.bottom) && StartRender(s);
        if (ok) Log(L"Monitor %ldx%ld: HAP (sin decodificador de video)", rc.right, rc.bottom);
    }
    if (!ok && g_dxgiMgr && GetFileAttributesW(g_videoPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        s->engine = CreateEngine(s->wnd, true);
        ok = s->engine && StartRender(s);
        if (ok) Log(L"Monitor %ldx%ld: video con Media Foundation", rc.right, rc.bottom);
    }
    if (!ok && !s->swap && GetFileAttributesW(g_videoPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (s->engine) { s->engine->Shutdown(); s->engine->Release(); }
        s->engine = CreateEngine(s->wnd, false);
        s->engineOwnsWindow = s->engine != nullptr;
    }
}

static BOOL CALLBACK EnumMonProc(HMONITOR mon, HDC, LPRECT, LPARAM lp) {
    AttachScreen((HWND)lp, mon);
    return TRUE;
}

static void Attach() {
    DestroyScreens();
    HWND worker = FindWorkerW();
    if (!worker) { Log(L"No se encontro WorkerW"); return; }
    if (!g_hapChecked) {
        g_hapChecked = true;
        if (!g_hapPath.empty() && !ValidateHap()) { g_hapFrames = 0; Log(L"Archivo HAP invalido: %ls", g_hapPath.c_str()); }
    }
    InitD3D(g_hapFrames == 0); // con HAP valido no hace falta soporte de video ni Media Foundation
    if (!g_hapPath.empty() && !InitHap() && !g_hapWarned) {
        g_hapWarned = true;
        bool mp4 = GetFileAttributesW(g_videoPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        Balloon(L"HapWallpaper", mp4 ? L"The .hap file could not be used; playing the MP4 instead. Recreate it with prepare_video.ps1."
                                     : L"The .hap file could not be used. Recreate it with prepare_video.ps1.");
    }
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, (LPARAM)worker);
    Log(L"Adjuntado a WorkerW %p, %u monitores", worker, (unsigned)g_screens.size());
}

// ------------------------------------------------------------- Auto-pausa
struct CoverCtx { HMONITOR mon; RECT work; HWND found; };

static BOOL CALLBACK CoverProc(HWND h, LPARAM lp) {
    CoverCtx* c = (CoverCtx*)lp;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) return TRUE;
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd")) return TRUE;
    if (MonitorFromWindow(h, MONITOR_DEFAULTTONULL) != c->mon) return TRUE;
    RECT r;
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(h, &r);
    // Pausa con ventanas maximizadas o a pantalla completa en ese monitor: detras de la
    // barra transparente queda el ultimo cuadro congelado.
    bool covers = r.left <= c->work.left && r.top <= c->work.top && r.right >= c->work.right && r.bottom >= c->work.bottom;
    if (covers) { c->found = h; return FALSE; }
    return TRUE;
}

static HWND CoveringWindow(Screen* s) {
    CoverCtx c = { s->mon, s->work, nullptr };
    EnumWindows(CoverProc, (LPARAM)&c);
    return c.found;
}

static bool FullscreenAppRunning() {
    QUERY_USER_NOTIFICATION_STATE st;
    if (FAILED(SHQueryUserNotificationState(&st))) return false;
    // QUNS_BUSY no se usa: Windows lo reporta aunque no haya nada en pantalla completa.
    return st == QUNS_RUNNING_D3D_FULL_SCREEN || st == QUNS_PRESENTATION_MODE;
}

static void UpdateTrayTip() {
    unsigned playing = 0;
    for (Screen* s : g_screens) if (!ShouldPause(s)) playing++;
    if (g_userPaused) swprintf(g_nid.szTip, 128, L"HapWallpaper - paused");
    else swprintf(g_nid.szTip, 128, L"HapWallpaper - playing on %u of %u monitors%ls", playing, (unsigned)g_screens.size(),
                  g_hapReady ? L" (HAP)" : L"");
    if (g_tbOwner) {
        size_t n = wcslen(g_nid.szTip);
        swprintf(g_nid.szTip + n, 128 - n, L"\nTaskbar: managed by %ls", g_tbOwner);
    }
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void ApplyState() {
    for (Screen* s : g_screens) {
        bool pause = ShouldPause(s);
        if (s->runEvent) { if (pause) ResetEvent(s->runEvent); else SetEvent(s->runEvent); }
        if (s->engine) {
            BOOL isPaused = s->engine->IsPaused();
            if (pause && !isPaused) s->engine->Pause();
            else if (!pause && isPaused) s->engine->Play();
        }
    }
    UpdateTrayTip();
}

static void Check() {
    bool lost = g_screens.empty();
    for (Screen* s : g_screens) if (!IsWindow(s->wnd)) lost = true;
    if (lost) { Log(L"Ventanas perdidas, reintentando"); Attach(); }
    g_fullscreenApp = !g_noAutoPause && FullscreenAppRunning();
    for (Screen* s : g_screens) {
        HWND cover = g_noAutoPause ? nullptr : CoveringWindow(s);
        bool ap = cover != nullptr;
        if (ap != s->autoPaused) {
            if (cover) { wchar_t t[128] = {}, c[64] = {}; GetWindowTextW(cover, t, 128); GetClassNameW(cover, c, 64); Log(L"Monitor tapado por [%ls] \"%ls\"", c, t); }
            s->autoPaused = ap;
        }
    }
    ApplyState();
}

// ------------------------------------------------------- Barra transparente
// Reemplazo de TranslucentTB: la ventana de la barra se vuelve transparente con
// SetWindowCompositionAttribute y HapWallpaperTB.dll (cargada en Explorer) oculta
// el fondo XAML y la linea superior.
struct AccentPolicy { int State, Flags, Color, Anim; };
struct WinCompAttrData { int Attr; void* Data; ULONG Size; };
typedef BOOL (WINAPI *PFN_SWCA)(HWND, WinCompAttrData*);
// La barra transparente se puede prender o apagar desde el menu de la bandeja (se guarda en
// HapWallpaper.ini junto al .exe). Sin eleccion guardada viene prendida, salvo que otro
// programa ya maneje la barra: ahi se queda apagada sin avisar. Apagada, no se toca Explorer.
static bool g_taskbar = false;
static bool g_taskbarAuto = true; // sin eleccion guardada en el .ini
static HHOOK g_tbHook;
static int g_hookTicks;
static DWORD g_injectedPid;
static int g_injectTries;
static HWINEVENTHOOK g_fgHook;

static void ClearTaskbarWindow(HWND h) {
    static PFN_SWCA set = (PFN_SWCA)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    static PFN_SWCA get = (PFN_SWCA)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowCompositionAttribute");
    if (!set) return;
    AccentPolicy cur = {};
    WinCompAttrData d = { 19 /* WCA_ACCENT_POLICY */, &cur, sizeof(cur) };
    if (get && get(h, &d) && cur.State == 2 && cur.Color == 0) return; // ya esta transparente
    AccentPolicy ap = { 2 /* ACCENT_ENABLE_TRANSPARENTGRADIENT */, 2, 0x00000000, 0 };
    d.Data = &ap;
    set(h, &d);
}

static void ApplyTaskbar() {
    if (!g_taskbar || g_tbOwner) return;
    if (HWND t = FindWindowW(L"Shell_TrayWnd", nullptr)) ClearTaskbarWindow(t);
    HWND s = nullptr;
    while ((s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr))) ClearTaskbarWindow(s);
}

// Otro programa que ya maneja la barra con la misma API de diagnostico XAML (solo
// puede haber uno por Explorer). Si lo hay, HapWallpaper no toca la barra para no
// romperlo, y avisa. Devuelve su nombre o nullptr.
static bool g_forceTaskbar;

static std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

static bool ProcessRunning(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = { sizeof(pe) };
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = !_wcsicmp(pe.szExeFile, exe);
    CloseHandle(snap);
    return found;
}

static bool WindhawkTaskbarModEnabled() {
    // Windhawk guarda cada mod en HKLM\SOFTWARE\Windhawk\Engine\Mods\<id> (instalacion normal).
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Windhawk\\Engine\\Mods", 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return false;
    bool found = false;
    wchar_t name[256];
    for (DWORD i = 0; !found; i++) {
        DWORD len = 256;
        if (RegEnumKeyExW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        std::wstring n = Lower(name);
        if (n.find(L"taskbar-styler") == std::wstring::npos) continue;
        DWORD disabled = 0, sz = sizeof(disabled);
        RegGetValueW(k, name, L"Disabled", RRF_RT_REG_DWORD, nullptr, &disabled, &sz);
        found = disabled == 0;
    }
    RegCloseKey(k);
    return found;
}

static const wchar_t* OtherTaskbarTool(DWORD explorerPid) {
    if (ProcessRunning(L"TranslucentTB.exe")) return L"TranslucentTB";
    // Modulos ya cargados en Explorer: el TAP de TranslucentTB o un mod de barra de Windhawk.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, explorerPid);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me = { sizeof(me) };
        const wchar_t* who = nullptr;
        for (BOOL ok = Module32FirstW(snap, &me); ok && !who; ok = Module32NextW(snap, &me)) {
            std::wstring path = Lower(me.szExePath), mod = Lower(me.szModule);
            if (mod == L"explorertap.dll") who = L"TranslucentTB";
            else if (path.find(L"windhawk") != std::wstring::npos && mod.find(L"taskbar") != std::wstring::npos) who = L"Windhawk (Taskbar Styler)";
        }
        CloseHandle(snap);
        if (who) return who;
    }
    if (ProcessRunning(L"windhawk.exe") && WindhawkTaskbarModEnabled()) return L"Windhawk (Taskbar Styler)";
    return nullptr;
}

static void InjectTaskbar() {
    if (!g_taskbar) return;
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(tray, &pid);
    static DWORD lastPid;
    if (pid != lastPid) { lastPid = pid; g_injectTries = 0; } // Explorer se reinicio
    if (!pid || pid == g_injectedPid) return;

    if (!g_forceTaskbar && !g_tbHook) {
        const wchar_t* owner = OtherTaskbarTool(pid);
        if (owner != g_tbOwner) {
            if (owner && g_taskbarAuto) {
                // Sin eleccion del usuario: no molestar, solo se ve en el tooltip.
                Log(L"La barra la maneja %ls: barra transparente apagada (automatico)", owner);
            } else if (owner) {
                Log(L"La barra la maneja %ls: no se carga HapWallpaperTB.dll", owner);
                std::wstring msg = std::wstring(L"Your taskbar is managed by ") + owner +
                    L", so HapWallpaper's transparent taskbar stays off (only one tool can change it). Use " + owner + L" to make it transparent.";
                Balloon(L"HapWallpaper", msg.c_str());
            }
            g_tbOwner = owner;
            UpdateTrayTip();
        }
        if (owner) return;
    }

    // Ya cargada en este Explorer (por ejemplo, si HapWallpaper se reinicio)?
    wchar_t evName[64];
    swprintf(evName, 64, L"Local\\HapWallpaperTB_%u", (unsigned)pid);
    if (HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, evName)) {
        CloseHandle(ev);
        if (g_tbHook) { UnhookWindowsHookEx(g_tbHook); g_tbHook = nullptr; }
        g_injectedPid = pid;
        Log(L"Barra lista en Explorer (pid %u)", (unsigned)pid);
        return;
    }
    if (g_tbHook) {
        // Esperando a que la DLL se registre; si tarda demasiado, reintentar.
        if (++g_hookTicks < 5) { PostMessageW(tray, WM_NULL, 0, 0); return; }
        UnhookWindowsHookEx(g_tbHook); g_tbHook = nullptr;
    }
    if (g_injectTries >= 30) return;
    std::wstring dll = g_exeDir + L"\\HapWallpaperTB.dll";
    static HMODULE mod = nullptr;
    if (!mod) mod = LoadLibraryW(dll.c_str());
    HOOKPROC proc = mod ? (HOOKPROC)GetProcAddress(mod, "FvGetMsgProc") : nullptr;
    if (!proc) { Log(L"No se pudo cargar HapWallpaperTB.dll"); g_injectTries = 30; return; }
    g_injectTries++;
    DWORD tid = GetWindowThreadProcessId(tray, nullptr);
    g_tbHook = SetWindowsHookExW(WH_GETMESSAGE, proc, mod, tid);
    g_hookTicks = 0;
    PostMessageW(tray, WM_NULL, 0, 0);
    Log(L"Hook en la barra (pid %u, intento %d): %ls", (unsigned)pid, g_injectTries, g_tbHook ? L"ok" : L"fallo");
}

static void CALLBACK ForegroundHook(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) { ApplyTaskbar(); }

static std::wstring IniPath() { return g_exeDir + L"\\HapWallpaper.ini"; }
// -1 = sin eleccion guardada (automatico), 0 = apagada, 1 = prendida.
static int LoadTaskbarSetting() { return (int)GetPrivateProfileIntW(L"Settings", L"TransparentTaskbar", -1, IniPath().c_str()); }
static void SaveTaskbarSetting(bool on) {
    WritePrivateProfileStringW(L"Settings", L"TransparentTaskbar", on ? L"1" : L"0", IniPath().c_str());
    g_taskbarAuto = false;
}

static void ResetTaskbarWindow(HWND h) {
    static PFN_SWCA set = (PFN_SWCA)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    if (!set) return;
    AccentPolicy ap = { 0 /* ACCENT_DISABLED: el estilo normal de Windows */, 0, 0, 0 };
    WinCompAttrData d = { 19, &ap, sizeof(ap) };
    set(h, &d);
}

// Opcion "Transparent taskbar" del menu de la bandeja.
static void ToggleTaskbar() {
    if (!g_taskbar || g_tbOwner) {
        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        DWORD pid = 0;
        if (tray) GetWindowThreadProcessId(tray, &pid);
        const wchar_t* owner = (!g_forceTaskbar && pid) ? OtherTaskbarTool(pid) : nullptr;
        if (owner) {
            std::wstring msg = std::wstring(L"The transparent taskbar isn't compatible with ") + owner +
                L": only one program can change the taskbar this way, and it's already in use.\n\n"
                L"Use " + owner + L" to make the taskbar transparent. The animated wallpaper works the same either way.";
            MessageBoxW(g_msgWnd, msg.c_str(), L"HapWallpaper", MB_ICONINFORMATION);
            return;
        }
        g_taskbar = true; g_tbOwner = nullptr; g_injectTries = 0;
        SaveTaskbarSetting(true);
        InjectTaskbar(); ApplyTaskbar();
    } else {
        g_taskbar = false;
        SaveTaskbarSetting(false);
        if (g_tbHook) { UnhookWindowsHookEx(g_tbHook); g_tbHook = nullptr; }
        if (HWND t = FindWindowW(L"Shell_TrayWnd", nullptr)) ResetTaskbarWindow(t);
        HWND s = nullptr;
        while ((s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr))) ResetTaskbarWindow(s);
        if (g_injectedPid)
            Balloon(L"HapWallpaper", L"Transparent taskbar off. The taskbar fully returns to normal the next time you sign in or Explorer restarts.");
    }
    UpdateTrayTip();
}

// ------------------------------------------------------------- Bandeja / salida
static void AddTray() {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_msgWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    // Icono chico de la bandeja al tamano justo para la escala de pantalla (16 px a 100 %, 24 a 150 %...).
    UINT dpi = GetDpiForSystem();
    int sz = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    static HICON trayIcon = nullptr;
    if (!trayIcon) trayIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, sz, sz, LR_DEFAULTCOLOR);
    g_nid.hIcon = trayIcon ? trayIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"HapWallpaper");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void ShowMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_TOGGLE, g_userPaused ? L"Resume" : L"Pause");
    AppendMenuW(m, MF_STRING | (g_taskbar && !g_tbOwner ? MF_CHECKED : 0), IDM_TASKBAR, L"Transparent taskbar");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(g_msgWnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, g_msgWnd, nullptr);
    DestroyMenu(m);
}

static void RestoreDesktop() {
    // Vuelve a aplicar el fondo estatico actual para que Explorer repinte la capa.
    wchar_t wp[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wp, 0);
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, wp, SPIF_SENDCHANGE);
}

static LRESULT CALLBACK MsgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TASKBARCREATED && WM_TASKBARCREATED) { Log(L"Explorer reiniciado"); AddTray(); Attach(); ApplyState(); return 0; }
    switch (msg) {
    case WM_TIMER: Check(); InjectTaskbar(); ApplyTaskbar(); return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowMenu();
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK) { g_userPaused = !g_userPaused; ApplyState(); }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDM_TOGGLE) { g_userPaused = !g_userPaused; ApplyState(); }
        else if (LOWORD(wp) == IDM_TASKBAR) ToggleTaskbar();
        else if (LOWORD(wp) == IDM_EXIT) DestroyWindow(h);
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) g_locked = true;
        else if (wp == WTS_SESSION_UNLOCK) g_locked = false;
        ApplyState();
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_POWERSETTINGCHANGE) {
            auto* ps = (POWERBROADCAST_SETTING*)lp;
            if (ps->PowerSetting == FV_GUID_CONSOLE_DISPLAY_STATE && ps->DataLength >= sizeof(DWORD)) {
                g_displayOff = *(DWORD*)ps->Data == 0;
                ApplyState();
            }
        }
        return TRUE;
    case WM_DISPLAYCHANGE: Attach(); ApplyState(); return 0; // monitores o resolucion cambiaron
    case WM_ENDSESSION:
        if (wp) DestroyScreens();
        return 0;
    case WM_DESTROY:
        KillTimer(h, IDT_CHECK);
        if (g_tbHook) UnhookWindowsHookEx(g_tbHook);
        if (g_fgHook) UnhookWinEvent(g_fgHook);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        DestroyScreens();
        RestoreDesktop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK VideoProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        Screen* s = (Screen*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (!s || (!s->swap && !s->engineOwnsWindow)) { RECT r; GetClientRect(h, &r); FillRect((HDC)wp, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); }
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"HapWallpaper_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InitializeCriticalSection(&g_ctxLock);

    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_exeDir = exe; g_exeDir = g_exeDir.substr(0, g_exeDir.find_last_of(L'\\'));
    // Nombres por defecto (y los anteriores en espanol, por compatibilidad).
    auto exists = [](const std::wstring& f) { return GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES; };
    g_videoPath = g_exeDir + L"\\wallpaper.mp4";
    if (!exists(g_videoPath) && exists(g_exeDir + L"\\fondo.mp4")) g_videoPath = g_exeDir + L"\\fondo.mp4";
    std::wstring defaultHap = g_exeDir + L"\\wallpaper.hap";
    if (!exists(defaultHap) && exists(g_exeDir + L"\\fondo.hap")) defaultHap = g_exeDir + L"\\fondo.hap";
    bool explicitPath = false;
    int tbSetting = LoadTaskbarSetting();
    g_taskbarAuto = tbSetting < 0;
    g_taskbar = tbSetting != 0;
    int argc; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--debug")) g_debug = true;
        else if (!wcscmp(argv[i], L"--nopause")) g_noAutoPause = true;
        else if (!wcscmp(argv[i], L"--notaskbar")) g_taskbar = false;
        else if (!wcscmp(argv[i], L"--forcetaskbar")) { g_forceTaskbar = true; g_taskbar = true; }
        else if (!wcscmp(argv[i], L"--noautostart")) g_noAutostart = true;
        else { g_videoPath = argv[i]; explicitPath = true; }
    }
    LocalFree(argv);
    // La DLL de la barra solo escribe tb_log.txt si existe este evento.
    HANDLE debugEvent = g_debug ? CreateEventW(nullptr, TRUE, TRUE, L"Local\\HapWallpaperDebug") : nullptr;

    // wallpaper.hap junto al .exe (o un .hap pasado como argumento) tiene prioridad sobre el video.
    if (g_videoPath.size() > 4 && !_wcsicmp(g_videoPath.c_str() + g_videoPath.size() - 4, L".hap")) g_hapPath = g_videoPath;
    else if (!explicitPath && exists(defaultHap)) g_hapPath = defaultHap;
    bool haveVideo = exists(g_videoPath);
    if (g_hapPath.empty() && !haveVideo) {
        MessageBoxW(nullptr, (L"Video not found:\n" + g_videoPath +
                              L"\n\nPut wallpaper.hap or wallpaper.mp4 next to HapWallpaper.exe (see prepare_video.ps1).").c_str(),
                    L"HapWallpaper", MB_ICONERROR);
        return 1;
    }
    Log(L"Inicio, hap: %ls, video: %ls", g_hapPath.empty() ? L"-" : g_hapPath.c_str(), haveVideo ? g_videoPath.c_str() : L"-");

    // Se anota solo en el inicio de Windows (HKCU\...\Run) con su ruta actual.
    if (!g_noAutostart) {
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            wchar_t cur[MAX_PATH * 2] = {};
            DWORD sz = sizeof(cur), type = 0;
            if (RegQueryValueExW(k, L"HapWallpaper", nullptr, &type, (BYTE*)cur, &sz) != ERROR_SUCCESS || cmd != cur)
                RegSetValueExW(k, L"HapWallpaper", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(k);
        }
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    WNDCLASSW wc = {};
    wc.hInstance = inst;
    wc.lpfnWndProc = MsgProc; wc.lpszClassName = L"HapWallpaperMsg";
    RegisterClassW(&wc);
    wc.lpfnWndProc = VideoProc; wc.lpszClassName = L"HapWallpaperVideo";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
    // Ventana oculta (no message-only) para recibir TaskbarCreated y avisos de energia.
    g_msgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"HapWallpaperMsg", L"HapWallpaper", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    WTSRegisterSessionNotification(g_msgWnd, NOTIFY_FOR_THIS_SESSION);
    RegisterPowerSettingNotification(g_msgWnd, &FV_GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);

    AddTray();
    Attach();
    Check();
    InjectTaskbar();
    ApplyTaskbar();
    // El hook es barato y ApplyTaskbar no hace nada con la opcion apagada; asi sirve si se activa despues.
    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ForegroundHook, 0, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(g_msgWnd, IDT_CHECK, 1000, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }

    WTSUnRegisterSessionNotification(g_msgWnd);
    CloseHap();
    if (g_sampler) g_sampler->Release();
    if (g_vs) g_vs->Release();
    if (g_ps) g_ps->Release();
    if (g_dxgiMgr) g_dxgiMgr->Release();
    if (g_d3d) g_d3d->Release();
    if (g_mfStarted) MFShutdown();
    CoUninitialize();
    if (debugEvent) CloseHandle(debugEvent);
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    return 0;
}
