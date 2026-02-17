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

// Глобальные переменные
JavaVM* g_vm = nullptr;
std::string GLOBAL_CACHE_DIR = "";
const std::string MOD_BASE_PATH = "/storage/emulated/0/Documents/LibL";



void loadExtraLibraries() {
    // Путь к нативным библиотекам приложения
    // В большинстве случаев это /data/data/название.пакета/lib или путь через /proc/self/exe
    // Но самый надежный способ для инжекта - прочитать свою папку из /proc/self/maps
    
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

    if (libDir.empty()) {
        LOGE("Could not find library directory");
        return;
    }

    LOGI("Scanning directory for 'Load' libraries: %s", libDir.c_str());

    DIR* dir = opendir(libDir.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            // Ищем "Load" в имени и проверяем что это .so
            if (strstr(entry->d_name, "Load") && strstr(entry->d_name, ".so")) {
                // Игнорируем саму себя, если вдруг в нашем имени есть "Load"
                if (strstr(entry->d_name, "native-lib")) continue; 

                std::string fullPath = libDir + "/" + entry->d_name;
                LOGI("Found extra library: %s", entry->d_name);
                
                void* handle = dlopen(fullPath.c_str(), RTLD_NOW);
                if (handle) {
                    LOGI("Successfully loaded: %s", entry->d_name);
                } else {
                    LOGE("Failed to load %s: %s", entry->d_name, dlerror());
                }
            }
        }
        closedir(dir);
    }
}




// --- Получение динамического пути кэша (важно для System.load в виртуалке) ---
std::string get_virtual_cache_dir(JNIEnv* env) {
    if (!env) return "";
    jclass activityThread = env->FindClass("android/app/ActivityThread");
    if (!activityThread) return "";
    jmethodID currentAppMethod = env->GetStaticMethodID(activityThread, "currentApplication", "()Landroid/app/Application;");
    jobject appObj = env->CallStaticObjectMethod(activityThread, currentAppMethod);
    if (!appObj) return "";

    jclass contextClass = env->FindClass("android/content/Context");
    jmethodID getCacheDirMethod = env->GetMethodID(contextClass, "getCacheDir", "()Ljava/io/File;");
    jobject cacheDirFile = env->CallObjectMethod(appObj, getCacheDirMethod);

    jclass fileClass = env->FindClass("java/io/File");
    jmethodID getAbsolutePathMethod = env->GetMethodID(fileClass, "getAbsolutePath", "()Ljava/lang/String;");
    jstring pathString = (jstring)env->CallObjectMethod(cacheDirFile, getAbsolutePathMethod);

    const char* pathChars = env->GetStringUTFChars(pathString, nullptr);
    std::string path(pathChars);
    env->ReleaseStringUTFChars(pathString, pathChars);
    return path;
}

// --- Инструментарий ModLoader ---
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
    int deletedCount = 0;
    while ((entry = readdir(dir)) != nullptr) {
        if (strstr(entry->d_name, "lib_") && strstr(entry->d_name, ".so")) {
            std::string fullPath = cachePath + "/" + entry->d_name;
            if (unlink(fullPath.c_str()) == 0) deletedCount++;
        }
    }
    closedir(dir);
    LOGI("[SNITY] Cache cleaned. Deleted %d old libs.", deletedCount);
}

void initSnityModLoader(std::string pkgName) {
    // В режиме System.load g_vm уже инициализирован в JNI_OnLoad
    if (GLOBAL_CACHE_DIR.empty() && g_vm) {
        JNIEnv* env = nullptr;
        g_vm->AttachCurrentThread(&env, nullptr);
        GLOBAL_CACHE_DIR = get_virtual_cache_dir(env);
    }

    if (GLOBAL_CACHE_DIR.empty()) {
        LOGE("[SNITY] Critical Error: Cache path detection failed!");
        return;
    }

    // 1. Очистка
    clearSnityCache(GLOBAL_CACHE_DIR);

#if defined(__aarch64__)
    std::string arch = "arm64-v8a";
#else
    std::string arch = "armeabi-v7a";
#endif

    std::string libDir = MOD_BASE_PATH + "/" + pkgName + "/lib/" + arch;
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
                    else LOGE("[SNITY] Load Error %s: %s", entry->d_name, dlerror());
                }
            }
        }
        closedir(dir);
    }
}

// --- Поток сканирования карт памяти и дампа ---
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

void dump_thread() {
    std::string pkgName = GetPackageName();
    
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
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
        GLOBAL_CACHE_DIR = get_virtual_cache_dir(env);
    }


    
    std::thread(dump_thread).detach();

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
    std::thread(dump_thread).detach();
}
#endif
