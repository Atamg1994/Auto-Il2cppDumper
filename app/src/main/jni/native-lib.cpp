#include <jni.h>
#include <string>
#include <thread>
#include <dlfcn.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/system_properties.h>
#include "Il2Cpp/il2cpp_dump.h"
#include "Includes/config.h"
#include "Includes/log.h"

// Функция для поиска и загрузки сторонних библиотек с "Load" в имени
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

#define libTarget "libil2cpp.so"

void dump_thread() {
    LOGI("Lib loaded");
    
    // Сначала загружаем доп. библиотеки, если они есть
    loadExtraLibraries();

    do {
        sleep(1);
    } while (!isLibraryLoaded(libTarget));

    LOGI("Waiting in %d...", Sleep);
    sleep(Sleep);

    auto il2cpp_handle = dlopen(libTarget, 4);
    LOGI("Start dumping");

    auto androidDataPath = std::string("/storage/emulated/0/Android/data/").append(
            GetPackageName()).append("/").append(GetPackageName()).append("-dump.cs");

    il2cpp_api_init(il2cpp_handle);
    il2cpp_dump(androidDataPath.c_str());
}

void *pLibRealUnity = 0;
typedef jint(JNICALL *CallJNI_OnLoad_t)(JavaVM *vm, void *reserved);
typedef void(JNICALL *CallJNI_OnUnload_t)(JavaVM *vm, void *reserved);
CallJNI_OnLoad_t RealJNIOnLoad = 0;
CallJNI_OnUnload_t RealJNIOnUnload = 0;

#ifdef UseFakeLib

JNIEXPORT jint JNICALL CallJNIOL(JavaVM *vm, void *reserved) {
    LOGI("OnLoad called");
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

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    LOGI("Initialize JNI");
    return CallJNIOL(vm, reserved);
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *reserved) {
    LOGI("Unload JNI");
    CallJNIUL(vm, reserved);
}

#else

__attribute__((constructor))
void lib_main() {
    std::thread(dump_thread).detach();
}
#endif
