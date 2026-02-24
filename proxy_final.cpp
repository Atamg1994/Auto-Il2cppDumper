#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>

static void* hOrig = nullptr;

__attribute__((constructor)) void init_proxy() {
    #if defined(__aarch64__)
        hOrig = dlopen("libkxqpplatform_real.so", RTLD_NOW);
    #else
        hOrig = dlopen("libkxqpplatform_32_real.so", RTLD_NOW);
    #endif

    // === ТВОЙ КОД ЗДЕСЬ ===
    __android_log_print(ANDROID_LOG_INFO, "PROXY", "INJECTED SUCCESS");
}

extern "C" void* RegisterNatives(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    typedef void* (*f_t)(void*, void*, void*, void*, void*, void*, void*, void*);
    static f_t o = nullptr;
    if (!o) o = (f_t)dlsym(hOrig, "RegisterNatives");
    return o ? o(a1, a2, a3, a4, a5, a6, a7, a8) : nullptr;
}

extern "C" void* dbt_hooker(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    typedef void* (*f_t)(void*, void*, void*, void*, void*, void*, void*, void*);
    static f_t o = nullptr;
    if (!o) o = (f_t)dlsym(hOrig, "dbt_hooker");
    return o ? o(a1, a2, a3, a4, a5, a6, a7, a8) : nullptr;
}

extern "C" void* gjvm(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    typedef void* (*f_t)(void*, void*, void*, void*, void*, void*, void*, void*);
    static f_t o = nullptr;
    if (!o) o = (f_t)dlsym(hOrig, "gjvm");
    return o ? o(a1, a2, a3, a4, a5, a6, a7, a8) : nullptr;
}

extern "C" void* hookCap(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    typedef void* (*f_t)(void*, void*, void*, void*, void*, void*, void*, void*);
    static f_t o = nullptr;
    if (!o) o = (f_t)dlsym(hOrig, "hookCap");
    return o ? o(a1, a2, a3, a4, a5, a6, a7, a8) : nullptr;
}

extern "C" void* hookCount(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    typedef void* (*f_t)(void*, void*, void*, void*, void*, void*, void*, void*);
    static f_t o = nullptr;
    if (!o) o = (f_t)dlsym(hOrig, "hookCount");
    return o ? o(a1, a2, a3, a4, a5, a6, a7, a8) : nullptr;
}

extern "C" void* mm_autotext(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    typedef void* (*f_t)(void*, void*, void*, void*, void*, void*, void*, void*);
    static f_t o = nullptr;
    if (!o) o = (f_t)dlsym(hOrig, "mm_autotext");
    return o ? o(a1, a2, a3, a4, a5, a6, a7, a8) : nullptr;
}

extern "C" void* mm_voice_changer(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    typedef void* (*f_t)(void*, void*, void*, void*, void*, void*, void*, void*);
    static f_t o = nullptr;
    if (!o) o = (f_t)dlsym(hOrig, "mm_voice_changer");
    return o ? o(a1, a2, a3, a4, a5, a6, a7, a8) : nullptr;
}

extern "C" void* orig_libc___system_property_find(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    typedef void* (*f_t)(void*, void*, void*, void*, void*, void*, void*, void*);
    static f_t o = nullptr;
    if (!o) o = (f_t)dlsym(hOrig, "orig_libc___system_property_find");
    return o ? o(a1, a2, a3, a4, a5, a6, a7, a8) : nullptr;
}

extern "C" void* ready_amrs(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    typedef void* (*f_t)(void*, void*, void*, void*, void*, void*, void*, void*);
    static f_t o = nullptr;
    if (!o) o = (f_t)dlsym(hOrig, "ready_amrs");
    return o ? o(a1, a2, a3, a4, a5, a6, a7, a8) : nullptr;
}

extern "C" void* sql_tracker(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    typedef void* (*f_t)(void*, void*, void*, void*, void*, void*, void*, void*);
    static f_t o = nullptr;
    if (!o) o = (f_t)dlsym(hOrig, "sql_tracker");
    return o ? o(a1, a2, a3, a4, a5, a6, a7, a8) : nullptr;
}
// Вариант 1 (обычный)
extern "C" JNIEXPORT void JNICALL Java_com_excelliance_kxqp_platform_ApplicationProxy_nativeOnLoad(void* env, void* thiz, void* s1, void* s2) {
    typedef void (*f_t)(void*, void*, void*, void*);
    static f_t o = (f_t)dlsym(hOrig, "Java_com_excelliance_kxqp_platform_ApplicationProxy_nativeOnLoad");
    if (o) o(env, thiz, s1, s2);
}

// Вариант 2 (с сигнатурой типов)
extern "C" JNIEXPORT void JNICALL Java_com_excelliance_kxqp_platform_ApplicationProxy_nativeOnLoad__Ljava_lang_String_2Ljava_lang_String_2(void* env, void* thiz, void* s1, void* s2) {
    // В оригинале это может быть та же самая функция или с таким же длинным именем
    // Сначала пробуем найти по длинному имени
    typedef void (*f_t)(void*, void*, void*, void*);
    static f_t o = (f_t)dlsym(hOrig, "Java_com_excelliance_kxqp_platform_ApplicationProxy_nativeOnLoad__Ljava_lang_String_2Ljava_lang_String_2");
    
    // Если по длинному не нашли, пробуем по короткому
    if (!o) o = (f_t)dlsym(hOrig, "Java_com_excelliance_kxqp_platform_ApplicationProxy_nativeOnLoad");
    
    if (o) o(env, thiz, s1, s2);
}
