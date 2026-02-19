#include <jni.h>
#include <memory>
#include <string>
#include <thread>
#include <dlfcn.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include <vector>
#include "Il2Cpp/il2cpp_dump.h"
#include "Includes/config.h"
#include "Includes/log.h"
#include "Includes/jni_bind_release.h"
#define TO_STR(view) std::string(view)
using namespace jni;

// --- Глобальные данные ---

JavaVM* g_vm_global = nullptr;
static std::unique_ptr<jni::JvmRef<jni::kDefaultJvm>> g_jvm;
std::string GLOBAL_CACHE_DIR = "";
std::string GLOBAL_PKG_NAME = "";
const std::string SD_ROOT = "/storage/emulated/0/Documents/SoLoader";

// --- Описание Java классов (JniBind 1.0.0 Beta) ---
// 1. Описываем Application (хотя бы просто имя)

// --- Описание Java классов (строго по правилам JniBind) ---
static constexpr Class kClassLoader{"java/lang/ClassLoader",
                                    Method{"toString", Return<jstring>{}, Params{}}
};

static constexpr Class kThread{"java/lang/Thread",
                               Static {
                                       Method{"currentThread", Return{Class{"java/lang/Thread"}}, Params{}}
                               },
                               Method{"getContextClassLoader", Return{kClassLoader}, Params{}}
};

static constexpr Class kFile{"java/io/File",
                             Method{"getAbsolutePath", Return<jstring>{}, Params{}}
};

static constexpr Class kContext{"android/content/Context",
                                Method{"getCacheDir", Return{kFile}, Params{}},
                                Method{"getPackageName", Return<jstring>{}, Params{}}
};

static constexpr Class kApplication{"android/app/Application"};

static constexpr Class kAppBindData{"android/app/ActivityThread$AppBindData",
                                    Field{"processName", jstring{}}
};

static constexpr Class kActivityThread{"android/app/ActivityThread",
                                       Static {
                                               Method{"currentApplication", Return{kApplication}, Params{}},
                                               Method{"currentActivityThread", Return{Class{"android/app/ActivityThread"}}, Params{}}
                                       },
        // Поле, содержащее данные о запущенном процессе
                                       Field{"mBoundApplication", kAppBindData}
};



// --- Проверка загрузки библиотеки ---

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

// --- Вспомогательные функции ---

void recursive_mkdir(std::string path) {
    std::stringstream ss(path);
    std::string item, current_path = "";
    while (std::getline(ss, item, '/')) {
        if (item.empty()) { current_path += "/"; continue; }
        current_path += item + "/";
        mkdir(current_path.c_str(), 0777);
    }
}

bool copyFile(const std::string& src, const std::string& dst) {
    std::ifstream source(src, std::ios::binary);
    std::ofstream dest(dst, std::ios::binary);
    if (!source.is_open() || !dest.is_open()) return false;
    dest << source.rdbuf();
    chmod(dst.c_str(), 0755);
    return true;
}

void waitAndLoadWorker(std::string fullPath, std::string targetLib, std::string fileName) {
    LOGI("[SoLoader] Thread started: waiting for %s to load %s", targetLib.c_str(), fileName.c_str());
    while (!isLibraryLoaded(targetLib.c_str())) {
        usleep(500000);
    }

    LOGI("[SoLoader] Target %s detected! Loading %s now...", targetLib.c_str(), fileName.c_str());
    void* handle = dlopen(fullPath.c_str(), RTLD_NOW);
    if (handle)
        LOGI("[SoLoader] Successfully loaded (Delayed): %s", fileName.c_str());
    else
        LOGE("[SoLoader] Failed to load %s: %s", fileName.c_str(), dlerror());
}

// --- Получение путей через JNI-Bind ---




void init_virtual_paths(JNIEnv* env) {
    int retry = 0;
    LOGI("[SoLoader] >>> Entering init_virtual_paths");

    while (retry < 100) {
        LOGI("[SoLoader] [Attempt %d] Searching for ActivityThread...", retry);

        // 1. Пытаемся получить доступ к ActivityThread
        auto activityThread = jni::StaticRef<kActivityThread>{};
        auto appJob = activityThread("currentApplication");
        LOGI("[SoLoader] [Attempt %d] currentApplication call finished", retry);

        if (static_cast<jobject>(appJob) != nullptr) {
            LOGI("[SoLoader] [Attempt %d] SUCCESS: appJob found!", retry);

            // Настройка Fallback Loader (для дочерних процессов GSpace)
            if (g_jvm) {
                LOGI("[SoLoader] Setting FallbackClassLoader from appJob...");
                g_jvm->SetFallbackClassLoaderFromJObject(static_cast<jobject>(appJob));
                LOGI("[SoLoader] FallbackClassLoader set.");
            } else {
                LOGE("[SoLoader] CRITICAL: g_jvm is NULL, skipping FallbackLoader!");
            }

            jni::LocalObject<kContext> app{std::move(appJob)};
            LOGI("[SoLoader] LocalObject<kContext> initialized");

            // --- Логика 1: Стандартные пути ---
            LOGI("[SoLoader] Requesting getPackageName...");
            auto pkgName = app("getPackageName");
            if (static_cast<jstring>(pkgName) != nullptr) {
                std::string tmpName = TO_STR(pkgName.Pin().ToString());

                // Если строки еще пустые — берем что дают (даже GSpace)
                // Если не пустые — обновляем только если нашли реальную игру
                if (GLOBAL_PKG_NAME.empty() || (tmpName.find("com.gspace.android") == std::string::npos)) {
                    GLOBAL_PKG_NAME = tmpName;
                }
                LOGI("[SoLoader] GLOBAL_PKG_NAME set to: %s", GLOBAL_PKG_NAME.c_str());
            }

            LOGI("[SoLoader] Requesting getCacheDir...");
            auto cacheFileObj = app("getCacheDir");
            if (static_cast<jobject>(cacheFileObj) != nullptr) {
                LOGI("[SoLoader] cacheFileObj obtained, converting to LocalObject...");
                jni::LocalObject<kFile> cacheFile{std::move(cacheFileObj)};
                auto pathString = cacheFile("getAbsolutePath");
                if (static_cast<jstring>(pathString) != nullptr) {
                    std::string tmpCache = TO_STR(pathString.Pin().ToString());

                    if (GLOBAL_CACHE_DIR.empty() || (tmpCache.find("/virtual/") != std::string::npos)) {
                        GLOBAL_CACHE_DIR = tmpCache;
                    }
                    LOGI("[SoLoader] GLOBAL_CACHE_DIR set to: %s", GLOBAL_CACHE_DIR.c_str());
                }
            }

            // --- Логика 2: Глубокий разбор ActivityThread (mBoundApplication) ---
            LOGI("[SoLoader] Checking mBoundApplication for real process name...");
            auto atObj = activityThread("currentActivityThread");
            if (static_cast<jobject>(atObj) != nullptr) {
                jni::LocalObject<kActivityThread> at{std::move(atObj)};
                // Вызываем строго по примеру: .Access<"name">().Get()
                auto bindDataRaw = at.Access<"mBoundApplication">().Get();

                if (static_cast<jobject>(bindDataRaw) != nullptr) {
                    LOGI("[SoLoader] AppBindData obtained!");

                    // Явно типизируем объект данных привязки
                    jni::LocalObject<kAppBindData> bindData{std::move(bindDataRaw)};

                    // Получаем processName
                    auto procNameJS = bindData.Access<"processName">().Get();

                    if (static_cast<jstring>(procNameJS) != nullptr) {
                        std::string realName = TO_STR(procNameJS.Pin().ToString());
                        LOGI("[SoLoader] Real Process Name: %s", realName.c_str());
                        // Если нашли имя без GSpace — это победа, переопределяем всё

                        if (!realName.empty() && realName.find("com.gspace.android") == std::string::npos) {
                            GLOBAL_PKG_NAME = realName;
                            LOGI("[SoLoader] !!! TARGET CAUGHT IN BIND DATA !!! PKG: %s", GLOBAL_PKG_NAME.c_str());
                        }
                    }
                }
            }

            // --- Логика 3: ClassLoader (permitted_path) ---
            LOGI("[SoLoader] Checking ClassLoader for virtual environment...");
            auto curThreadJob = jni::StaticRef<kThread>{}("currentThread");
            if (static_cast<jobject>(curThreadJob) != nullptr) {
                LOGI("[SoLoader] currentThread found, getting ClassLoader...");
                jni::LocalObject<kThread> curThread{std::move(curThreadJob)};
                auto loaderJob = curThread("getContextClassLoader");

                if (static_cast<jobject>(loaderJob) != nullptr) {
                    LOGI("[SoLoader] ClassLoader found, calling toString...");
                    jni::LocalObject<kClassLoader> loader{std::move(loaderJob)};

                    std::string loaderInfo { std::string(loader("toString").Pin().ToString()) };
                    LOGI("[SoLoader] Loader String: %s", loaderInfo.c_str());

                    size_t pPos = loaderInfo.find("permitted_path=");
                    if (pPos != std::string::npos) {
                        LOGI("[SoLoader] 'permitted_path' keyword detected, parsing...");
                        std::string pPath = loaderInfo.substr(pPos + 15);
                        size_t end = pPath.find_first_of(", \n]");
                        if (end != std::string::npos) pPath = pPath.substr(0, end);

                        size_t lastColon = pPath.find_last_of(':');
                        std::string targetPath = (lastColon != std::string::npos) ? pPath.substr(lastColon + 1) : pPath;
                        LOGI("[SoLoader] Extracted target path: %s", targetPath.c_str());

                        if (targetPath.find("/virtual/") != std::string::npos) {
                            // Если нашли виртуальный путь — это 100% гость, выходим
                            LOGI("[SoLoader] GSpace Virtual Path detected!");
                            GLOBAL_PKG_NAME = targetPath.substr(targetPath.find_last_of('/') + 1);
                            GLOBAL_CACHE_DIR = targetPath + "/cache";

                            LOGI("[SoLoader] !!! GSpace OVERRIDE !!! PKG: %s | Cache: %s",
                                 GLOBAL_PKG_NAME.c_str(), GLOBAL_CACHE_DIR.c_str());

                            return;
                        }
                    }
                }
            }

            // Финальная проверка для обычных процессов
            // УСЛОВИЕ ВЫХОДА:
            // Если нашли имя БЕЗ GSpace — выходим.
            // Если прошло 100 итераций и всё еще GSpace — выходим с тем, что есть.
            if (!GLOBAL_PKG_NAME.empty() && GLOBAL_PKG_NAME.find("com.gspace.android") == std::string::npos) {
                LOGI("[SoLoader] <<< SUCCESS: Guest app identified: %s", GLOBAL_PKG_NAME.c_str());
                return;


            }
        } else {
            LOGW("[SoLoader] appJob is NULL, ActivityThread not ready.");
        }

        usleep(500000);
        retry++;
    }
    LOGE("[SoLoader] !!! FAILED to init virtual paths after 100 retries !!!");
}







void processAndLoad(std::string fullPath, std::string fileName, bool isExternal) {
    std::string finalPath = fullPath;

    if (isExternal) {
        if (GLOBAL_CACHE_DIR.empty()) return;

        finalPath = GLOBAL_CACHE_DIR + "/" + fileName;

        if (!copyFile(fullPath, finalPath)) {
            LOGE("[SoLoader] Copy failed: %s", fileName.c_str());
            return;
        }
    }

    if (fileName.find("libWL_") == 0) {
        size_t first = fileName.find("_");
        size_t second = fileName.find("_", first + 1);

        if (first != std::string::npos && second != std::string::npos) {
            std::string target = fileName.substr(first + 1, second - first - 1);
            std::thread(waitAndLoadWorker, finalPath, target, fileName).detach();
        }

    } else if (fileName.find("Load") != std::string::npos) {

        void* h = dlopen(finalPath.c_str(), RTLD_NOW);
        if (h)
            LOGI("[SoLoader] Loaded: %s", fileName.c_str());
        else
            LOGE("[SoLoader] Load Error %s: %s", fileName.c_str(), dlerror());
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
                size_t s = path.find("/");
                size_t e = path.find_last_of("/");
                libDir = path.substr(s, e - s);
                break;
            }
        }
        fclose(fp);
    }

    if (!libDir.empty()) {
        DIR* dir = opendir(libDir.c_str());
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (strstr(entry->d_name, "native-lib") || !strstr(entry->d_name, ".so"))
                    continue;

                processAndLoad(libDir + "/" + entry->d_name, entry->d_name, false);
            }
            closedir(dir);
        }
    }

    if (GLOBAL_PKG_NAME.empty()) return;

#if defined(__aarch64__)
    std::string arch = "arm64-v8a";
#else
    std::string arch = "armeabi-v7a";
#endif

    std::string extPath = SD_ROOT + "/" + GLOBAL_PKG_NAME + "/lib/" + arch;
    recursive_mkdir(extPath);

    DIR* edir = opendir(extPath.c_str());
    if (edir) {
        struct dirent* entry;
        while ((entry = readdir(edir)) != nullptr) {
            if (!strstr(entry->d_name, ".so")) continue;
            processAndLoad(extPath + "/" + entry->d_name, entry->d_name, true);
        }
        closedir(edir);
    }
}

#define libTarget "libil2cpp.so"

void dump_thread() {

    JNIEnv* env;
    if (g_vm_global->AttachCurrentThread(&env, nullptr) != JNI_OK)
        return;
    // ВОТ ЭТА СТРОКА: Инициализирует JniBind для этого потока
    LOGI("[SoLoader] jni::ThreadGuard guard");
    jni::ThreadGuard guard;
    LOGI("[SoLoader] run dump_thread-> init_virtual_paths");
    init_virtual_paths(env);

    LOGI("[SoLoader] run dump_thread-> loadExtraLibraries");
    loadExtraLibraries();

    bool dump = true;
    int timeout = 120;
    int elapsed = 0;

    while (!isLibraryLoaded(libTarget)) {
        if (elapsed >= timeout) {
            LOGI("[SoLoader] Timeout reached: %s not found.", libTarget);
            dump = false;
            break;
        }
        sleep(1);
        elapsed++;
    }

    if (dump) {
        LOGI("[SoLoader] %s detected. Sleeping before dump...", libTarget);
        sleep(Sleep);

        void* il2cpp_handle = dlopen(libTarget, RTLD_NOW);
        if (il2cpp_handle) {

            std::string androidDataPath =
                    "/storage/emulated/0/Documents/" + GLOBAL_PKG_NAME + "-dump.cs";

            if (il2cpp_api_init(il2cpp_handle)) {
                il2cpp_dump(androidDataPath.c_str());
            }
        }
    }
    
    g_vm_global->DetachCurrentThread();
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {

    g_vm_global = vm;
    g_jvm = std::make_unique<jni::JvmRef<jni::kDefaultJvm>>(vm);
    // Инициализируем через reset, как в примере Google
    //g_jvm.reset(new jni::JvmRef<jni::kDefaultJvm>{vm});
    static jni::JvmRef<jni::kDefaultJvm> jvm{vm};

    LOGI("[SoLoader] JNI System Initialized (Release 1.5.0)");

    std::thread(dump_thread).detach();

    return JNI_VERSION_1_6;
}
