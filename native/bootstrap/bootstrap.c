// 引导 DLL：被手动映射进游戏进程后，由 CreateRemoteThread 直接调用 HanBootstrap。
// 刻意不用 CRT（-nostdlib），只 import kernel32 —— 手动映射时导入表最好解析。
// 它干的事：拿到 mono 导出 → 把「内存中的托管程序集」加载进 Mono → 反射调用 Injector.Start(cfg)。
#include <windows.h>

typedef void* (*Fn_RootDomain)(void);
typedef void (*Fn_Attach)(void*);
typedef void* (*Fn_ImgOpen)(char*, unsigned int, int, void*, unsigned int, const char*);
typedef void* (*Fn_AsmLoad)(void*, const char*, int*, unsigned int);
typedef void* (*Fn_AsmImg)(void*);
typedef void* (*Fn_ClassFromName)(void*, const char*, const char*);
typedef void* (*Fn_ClassMethod)(void*, const char*, int);
typedef void* (*Fn_StrNew)(void*, const char*);
typedef void (*Fn_Invoke)(void*, void*, void**, void**);

typedef struct {
    wchar_t monoDll[512];    // mono-2.0-bdwgc.dll 的绝对路径
    void*   managedBytes;    // 已写进本进程的托管 DLL 字节
    int     managedLen;
    void*   cfgBytes;        // 已写进本进程的 UTF-8 JSON（NUL 结尾）
    int     cfgLen;
    char    asmName[256];    // "SFSHanRuntime.dll"
    char    typeNs[256];     // "SFSHanRuntime"
    char    typeName[256];   // "Injector"
    char    methodName[128]; // "Start"
    wchar_t logPath[512];    // 注入日志
} HAN_PARAMS;

static int han_strlen(const char* s)
{
    int n = 0;
    while (s && s[n]) ++n;
    return n;
}

static void han_log(const wchar_t* logPath, const char* msg)
{
    if (!logPath || !logPath[0]) return;
    HANDLE h = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, 0,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    SetFilePointer(h, 0, 0, FILE_END);
    DWORD wrote = 0;
    WriteFile(h, msg, (DWORD)han_strlen(msg), &wrote, 0);
    WriteFile(h, "\r\n", 2, &wrote, 0);
    CloseHandle(h);
}

__declspec(dllexport) DWORD WINAPI HanBootstrap(HAN_PARAMS* p)
{
    if (!p) return 1;
    han_log(p->logPath, "bootstrap entered");

    HMODULE mono = LoadLibraryW(p->monoDll);
    if (!mono) { han_log(p->logPath, "ERR load mono dll"); return 2; }

    Fn_RootDomain f_root = (Fn_RootDomain)GetProcAddress(mono, "mono_get_root_domain");
    Fn_Attach    f_attach = (Fn_Attach)GetProcAddress(mono, "mono_thread_attach");
    Fn_ImgOpen   f_imgopen = (Fn_ImgOpen)GetProcAddress(mono, "mono_image_open_from_data_with_name");
    Fn_AsmLoad   f_asmload = (Fn_AsmLoad)GetProcAddress(mono, "mono_assembly_load_from_full");
    Fn_AsmImg    f_asmimg = (Fn_AsmImg)GetProcAddress(mono, "mono_assembly_get_image");
    Fn_ClassFromName f_class = (Fn_ClassFromName)GetProcAddress(mono, "mono_class_from_name");
    Fn_ClassMethod   f_method = (Fn_ClassMethod)GetProcAddress(mono, "mono_class_get_method_from_name");
    Fn_StrNew    f_strnew = (Fn_StrNew)GetProcAddress(mono, "mono_string_new");
    Fn_Invoke    f_invoke = (Fn_Invoke)GetProcAddress(mono, "mono_runtime_invoke");

    if (!f_root || !f_attach || !f_imgopen || !f_asmload || !f_asmimg || !f_class || !f_method ||
        !f_strnew || !f_invoke) {
        han_log(p->logPath, "ERR resolve mono exports");
        return 3;
    }

    {
        char buf[600];
        int i = 0;
        const char* pre = "params: ns=[";
        while (pre[i]) { buf[i] = pre[i]; ++i; }
        int k = 0; while (p->typeNs[k] && i < 200) buf[i++] = p->typeNs[k++];
        buf[i++]=']'; buf[i++]=' ';
        const char* pre2 = "type=[";
        k = 0; while (pre2[k] && i < 220) buf[i++] = pre2[k++];
        k = 0; while (p->typeName[k] && i < 320) buf[i++] = p->typeName[k++];
        buf[i++]=']'; buf[i++]=' ';
        const char* pre3 = "method=[";
        k = 0; while (pre3[k] && i < 340) buf[i++] = pre3[k++];
        k = 0; while (p->methodName[k] && i < 440) buf[i++] = p->methodName[k++];
        buf[i++]=']';
        buf[i]=0;
        han_log(p->logPath, buf);
    }
    void* domain = f_root();
    if (!domain) { han_log(p->logPath, "ERR no mono domain"); return 4; }
    f_attach(domain);
    han_log(p->logPath, "mono attached");

    int status = 0;
    void* image = f_imgopen((char*)p->managedBytes, (unsigned)p->managedLen, 0, &status, 0, p->asmName);
    if (!image) { han_log(p->logPath, "ERR image_open"); return 5; }

    void* assembly = f_asmload(image, p->asmName, &status, 0);
    if (!assembly) { han_log(p->logPath, "ERR assembly_load"); return 6; }

    void* img2 = f_asmimg(assembly);
    han_log(p->logPath, "assembly loaded");
    {
        typedef const char* (*Fn_ImgName)(void*);
        Fn_ImgName f_imgname = (Fn_ImgName)GetProcAddress(mono, "mono_image_get_name");
        if (f_imgname) {
            const char* n1 = f_imgname(image);
            const char* n2 = f_imgname(img2);
            char buf[512];
            int i = 0;
            const char* pre = "image=";
            while (pre[i]) { buf[i] = pre[i]; ++i; }
            if (n1) { int k = 0; while (n1[k] && i < 480) { buf[i++] = n1[k++]; } }
            const char* mid = " image2=";
            int m = 0; while (mid[m] && i < 490) { buf[i++] = mid[m++]; }
            if (n2) { int k = 0; while (n2[k] && i < 505) { buf[i++] = n2[k++]; } }
            buf[i] = 0;
            han_log(p->logPath, buf);
        }
    }
    // 先在「image_open 得到的镜像」上找类型，再退到 assembly 的镜像
    void* klass = f_class(image, p->typeNs, p->typeName);
    if (!klass) klass = f_class(image, "", p->typeName);
    if (!klass) klass = f_class(img2, p->typeNs, p->typeName);
    if (!klass) {
        han_log(p->logPath, "class lookup(ns) failed, trying empty ns");
        klass = f_class(img2, "", p->typeName);
    }
    if (!klass) { han_log(p->logPath, "ERR class not found"); return 7; }

    void* meth = f_method(klass, p->methodName, 1);
    if (!meth) { han_log(p->logPath, "ERR method not found"); return 8; }

    void* arg = f_strnew(domain, (const char*)p->cfgBytes);
    void* args[1];
    args[0] = arg;
    void* exc = 0;
    f_invoke(meth, 0, args, &exc);
    if (exc) { han_log(p->logPath, "ERR exception in Injector.Start"); return 9; }

    han_log(p->logPath, "OK injected");
    return 0;
}
