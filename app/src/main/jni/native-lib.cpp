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
#include <thread>
#include <string>
#include <vector>


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
// Функция-воркер для ожидания конкретной либы и последующей загрузки
void waitAndLoadWorker(std::string fullPath, std::string targetLib, std::string fileName) {
    LOGI("Thread started: waiting for %s to load %s", targetLib.c_str(), fileName.c_str());
    
    // Ждем, пока целевая библиотека появится в памяти
    while (!isLibraryLoaded(targetLib.c_str())) {
        usleep(500000); // 200мс
    }

    LOGI("Target %s detected! Loading %s now...", targetLib.c_str(), fileName.c_str());
    
    void* handle = dlopen(fullPath.c_str(), RTLD_NOW);
    if (handle) {
        LOGI("Successfully loaded (Delayed): %s", fileName.c_str());
    } else {
        LOGE("Failed to load %s: %s", fileName.c_str(), dlerror());
    }
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

    if (libDir.empty()) {
        LOGE("Could not find library directory");
        return;
    }

    LOGI("Scanning directory: %s", libDir.c_str());

    DIR* dir = opendir(libDir.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string fileName = entry->d_name;

            // Игнорируем себя
            if (fileName.find("native-lib") != std::string::npos) continue;
            if (fileName.find(".so") == std::string::npos) continue;

            std::string fullPath = libDir + "/" + fileName;

            // ВАРИАНТ 1: Ожидание (libWL_цель.so_название.so)
            if (fileName.find("libWL_") == 0) {
                size_t firstUnder = fileName.find("_");
                size_t secondUnder = fileName.find("_", firstUnder + 1);

                if (firstUnder != std::string::npos && secondUnder != std::string::npos) {
                    // Извлекаем "libil2cpp.so" из "libWL_libil2cpp.so_Mod.so"
                    std::string targetLib = fileName.substr(firstUnder + 1, secondUnder - firstUnder - 1);
                    
                    // Запускаем ожидание в отдельном потоке, чтобы не блокировать поиск других файлов
                    std::thread(waitAndLoadWorker, fullPath, targetLib, fileName).detach();
                    continue; 
                }
            }

            // ВАРИАНТ 2: Обычная немедленная загрузка (просто "Load" в имени)
            if (fileName.find("Load") != std::string::npos) {
                LOGI("Immediate load: %s", fileName.c_str());
                void* handle = dlopen(fullPath.c_str(), RTLD_NOW);
                if (handle) {
                    LOGI("Successfully loaded: %s", fileName.c_str());
                } else {
                    LOGE("Failed to load %s: %s", fileName.c_str(), dlerror());
                }
            }
        }
        closedir(dir);
    }
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

    if(il2cpp_api_init(il2cpp_handle)){
    il2cpp_dump(androidDataPath.c_str());
    }
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
