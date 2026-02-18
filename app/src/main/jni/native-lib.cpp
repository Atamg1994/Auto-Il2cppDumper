#include <jni.h>
#include <string>
#include <thread>
#include <dlfcn.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fstream>

#include "Il2Cpp/il2cpp_dump.h"
#include "Includes/config.h"
#include "Includes/log.h"
#include <errno.h>
#include <jni.h>
#include <string>
#include <vector>
#include <sstream>
#include <jni.h>
#include <string>
#include <thread>
#include <dlfcn.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fstream>
#include <errno.h>
#include <vector>
#include <sstream>

#include "Il2Cpp/il2cpp_dump.h"
#include "Includes/config.h"
#include "Includes/log.h"

// --- Глобальные переменные ---
JavaVM* g_vm = nullptr;
std::string pkgNameGlobal = "";
const std::string MOD_BASE_PATH = "/storage/emulated/0/Documents/LibL";
std::string GLOBAL_CACHE_DIR = "";
std::string GLOBAL_FILES_DIR = "";
std::string GLOBAL_EXTERNAL_FILES_DIR = ""; 

// --- Функция получения имени пакета через JNI (как во Frida) ---
std::string get_jni_package_name(JNIEnv* env, jobject appObj) {
    if (!appObj) return "";
    jclass contextClass = env->GetObjectClass(appObj);
    jmethodID getPkgMethod = env->GetMethodID(contextClass, "getPackageName", "()Ljava/lang/String;");
    jstring pkgNameStr = (jstring)env->CallObjectMethod(appObj, getPkgMethod);
    const char* pChars = env->GetStringUTFChars(pkgNameStr, nullptr);
    std::string result(pChars);
    env->ReleaseStringUTFChars(pkgNameStr, pChars);
    return result;
}

// --- Динамическое получение всех путей через JNI ---
void init_virtual_paths(JNIEnv* env) {
    jclass activityThread = env->FindClass("android/app/ActivityThread");
    jmethodID currentAppMethod = env->GetStaticMethodID(activityThread, "currentApplication", "()Landroid/app/Application;");
    
    jobject appObj = nullptr;
    for (int i = 0; i < 20; i++) {
        appObj = env->CallStaticObjectMethod(activityThread, currentAppMethod);
        if (appObj) break;
        usleep(200000);
    }

    if (!appObj) return;

    // Сразу получаем реальное имя пакета
    pkgNameGlobal = get_jni_package_name(env, appObj);

    jclass contextClass = env->FindClass("android/content/Context");
    jclass fileClass = env->FindClass("java/io/File");
    jmethodID getPathMethod = env->GetMethodID(fileClass, "getAbsolutePath", "()Ljava/lang/String;");

    // 1. Внутренний Кеш
    jmethodID getCacheDir = env->GetMethodID(contextClass, "getCacheDir", "()Ljava/io/File;");
    jobject cacheDirFile = env->CallObjectMethod(appObj, getCacheDir);
    jstring cachePath = (jstring)env->CallObjectMethod(cacheDirFile, getPathMethod);
    const char* cPath = env->GetStringUTFChars(cachePath, nullptr);
    GLOBAL_CACHE_DIR = std::string(cPath);
    env->ReleaseStringUTFChars(cachePath, cPath);

    // 2. Внутренние Файлы
    jmethodID getFilesDir = env->GetMethodID(contextClass, "getFilesDir", "()Ljava/io/File;");
    jobject filesDirFile = env->CallObjectMethod(appObj, getFilesDir);
    jstring filesPath = (jstring)env->CallObjectMethod(filesDirFile, getPathMethod);
    const char* fPath = env->GetStringUTFChars(filesPath, nullptr);
    GLOBAL_FILES_DIR = std::string(fPath);
    env->ReleaseStringUTFChars(filesPath, fPath);

    // 3. Путь Android/data
    jmethodID getExtFilesDir = env->GetMethodID(contextClass, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;");
    jobject extFilesDirFile = env->CallObjectMethod(appObj, getExtFilesDir, nullptr);
    if (extFilesDirFile) {
        jstring extPath = (jstring)env->CallObjectMethod(extFilesDirFile, getPathMethod);
        const char* ePath = env->GetStringUTFChars(extPath, nullptr);
        GLOBAL_EXTERNAL_FILES_DIR = std::string(ePath);
        env->ReleaseStringUTFChars(extPath, ePath);
    }
}

// --- Утилиты ---
bool isLibraryLoaded(const char *libraryName) {
    char line[512] = {0};
    FILE *fp = fopen("/proc/self/maps", "rt");
    if (fp != nullptr) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, libraryName)) {
                fclose(fp);
                return true;
            }
        }
        fclose(fp);
    }
    return false;
}

void recursive_mkdir(std::string path) {
    std::string current_level = "";
    std::string level;
    std::stringstream ss(path);
    while (std::getline(ss, level, '/')) {
        if (level.empty()) {
            current_level += "/";
            continue;
        }
        current_level += level + "/";
        mkdir(current_level.c_str(), 0777);
    }
}

bool snityCopyFile(const std::string& src, const std::string& dst) {
    std::ifstream source(src, std::ios::binary);
    std::ofstream dest(dst, std::ios::binary);
    if (!source.is_open() || !dest.is_open()) return false;
    dest << source.rdbuf();
    chmod(dst.c_str(), 0755); 
    return true;
}

void clearSnityCache(const std::string& cachePath) {
    DIR* dir = opendir(cachePath.c_str());
    if (!dir) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strstr(entry->d_name, "lib_") && strstr(entry->d_name, ".so")) {
            std::string fullPath = cachePath + "/" + entry->d_name;
            unlink(fullPath.c_str());
        }
    }
    closedir(dir);
    LOGI("[SNITY] Cache cleaned.");
}

void loadExtraLibraries() {
    char line[512];
    std::string libDir = "";
    FILE *fp = fopen("/proc/self/maps", "rt");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "/lib/") && strstr(line, ".so")) {
                std::string path = line;
                size_t start = path.find("/");
                size_t end = path.find_last_of("/");
                if (start != std::string::npos && end != std::string::npos) {
                    libDir = path.substr(start, end - start);
                    break;
                }
            }
        }
        fclose(fp);
    }
    if (libDir.empty()) return;
    DIR* dir = opendir(libDir.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strstr(entry->d_name, "Load") && strstr(entry->d_name, ".so")) {
                if (strstr(entry->d_name, "native-lib")) continue; 
                dlopen((libDir + "/" + entry->d_name).c_str(), RTLD_NOW);
            }
        }
        closedir(dir);
    }
}

// --- ModLoader ---
void initSnityModLoader(std::string pName) {
    if (GLOBAL_CACHE_DIR.empty()) return;
    clearSnityCache(GLOBAL_CACHE_DIR);

#if defined(__aarch64__)
    std::string arch = "arm64-v8a";
#else
    std::string arch = "armeabi-v7a";
#endif

    std::string libDir = MOD_BASE_PATH + "/" + pName + "/lib/" + arch;
    recursive_mkdir(libDir);

    LOGI("[SNITY] Cache Path: %s", GLOBAL_CACHE_DIR.c_str());
    LOGI("[SNITY] Mod Dir: %s", libDir.c_str());

    std::string pidStr = std::to_string(getpid());
    DIR* dir = opendir(libDir.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strstr(entry->d_name, ".so")) {
                std::string src = libDir + "/" + entry->d_name;
                std::string dst = GLOBAL_CACHE_DIR + "/lib_" + pidStr + "_" + entry->d_name;
                if (snityCopyFile(src, dst)) {
                    void* handle = dlopen(dst.c_str(), RTLD_NOW);
                    if (handle) LOGI("[SNITY] Loaded: %s", entry->d_name);
                }
            }
        }
        closedir(dir);
    }
}

// --- Threads ---
void dump_thread_v2() {
    JNIEnv* env = nullptr;
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;

    init_virtual_paths(env);
    
    if (GLOBAL_CACHE_DIR.empty()) {
        LOGE("[SNITY] ОШИБКА: Не удалось получить пути!");
        g_vm->DetachCurrentThread();
        return;
    }

    // Используем динамически полученное имя
    std::string currentPkg = pkgNameGlobal; 

    LOGI("[SNITY] Active Package: %s", currentPkg.c_str());
    
    loadExtraLibraries();
    initSnityModLoader(currentPkg);

    while (!isLibraryLoaded("libil2cpp.so")) {
        sleep(1);
    }
    sleep(Sleep);

    auto handle = dlopen("libil2cpp.so", RTLD_NOW);
    if (handle) {
        std::string dumpPath;
        if (!GLOBAL_EXTERNAL_FILES_DIR.empty()) {
            // Android/data/pkg/files -> Android/data/pkg/dump.cs
            std::string basePath = GLOBAL_EXTERNAL_FILES_DIR;
            size_t last = basePath.find_last_of('/');
            if (last != std::string::npos) basePath = basePath.substr(0, last);
            dumpPath = basePath + "/" + currentPkg + "-dump.cs";
        } else {
            dumpPath = "/storage/emulated/0/Documents/LibL/" + currentPkg + "-dump.cs";
        }
        
        LOGI("[SNITY] Dumping to: %s", dumpPath.c_str());
        il2cpp_api_init(handle);
        il2cpp_dump(dumpPath.c_str());
    }
    
    g_vm->DetachCurrentThread();
}


// --- Поток сканирования карт памяти и дампа ---


void dump_thread() {
    
    pkgName = GetPackageName();
    
    // Загружаем моды сразу после срабатывания System.load
    
    loadExtraLibraries();
    initSnityModLoader(pkgName);

    while (!isLibraryLoaded("libil2cpp.so")) {
        sleep(1);
    }

    LOGI("libil2cpp.so detected. Waiting %d...", Sleep);
    sleep(Sleep);

    auto handle = dlopen("libil2cpp.so", RTLD_NOW);
    if (handle) {
        auto dumpPath = std::string("/storage/emulated/0/Android/data/")
                .append(pkgName).append("/")
                .append(pkgName).append("-dump.cs");
        
        il2cpp_api_init(handle);
        il2cpp_dump(dumpPath.c_str());
        LOGI("Dump success: %s", dumpPath.c_str());
    }
}


// --- Точка входа System.load ---

void *pLibRealUnity = 0;
typedef jint(JNICALL *CallJNI_OnLoad_t)(JavaVM *vm, void *reserved);
typedef void(JNICALL *CallJNI_OnUnload_t)(JavaVM *vm, void *reserved);
CallJNI_OnLoad_t RealJNIOnLoad = 0;
CallJNI_OnUnload_t RealJNIOnUnload = 0;

JNIEXPORT jint JNICALL CallJNIOL(JavaVM *vm, void *reserved) {
    LOGI("OnLoad called");

    g_vm = vm;
    /*
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
        GLOBAL_CACHE_DIR = get_virtual_cache_dir(env);
    }
  */

    
    std::thread(dump_thread_v2).detach();

    if (!pLibRealUnity) pLibRealUnity = dlopen("librealmain.so", RTLD_NOW);
    if (!pLibRealUnity) pLibRealUnity = dlopen("librealunity.so", RTLD_NOW);
    
    if (pLibRealUnity && !RealJNIOnLoad) {
        RealJNIOnLoad = reinterpret_cast<CallJNI_OnLoad_t>(dlsym(pLibRealUnity, "JNI_OnLoad"));
    }
    
    return (RealJNIOnLoad) ? RealJNIOnLoad(vm, reserved) : JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL CallJNIUL(JavaVM *vm, void *reserved) {
    LOGI("OnUnload called");
    if (pLibRealUnity && !RealJNIOnUnload) {
        RealJNIOnUnload = reinterpret_cast<CallJNI_OnUnload_t>(dlsym(pLibRealUnity, "JNI_OnUnload"));
    }
    if (RealJNIOnUnload) RealJNIOnUnload(vm, reserved);
}


#ifdef UseFakeLib


JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    LOGI("Initialize JNI");
    return CallJNIOL(vm, reserved);
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved) {
    LOGI("Unload JNI");
    CallJNIUL(vm, reserved);
}

#else

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    LOGI("Initialize JNI");
    return CallJNIOL(vm, reserved);
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved) {
    LOGI("Unload JNI");
    CallJNIUL(vm, reserved);
}


__attribute__((constructor))
void lib_main() {
    if (!g_vm) {
        std::thread(dump_thread).detach();
    };
}
#endif
