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
#include "Includes/RemapTools.h"
#include <sys/syscall.h>
#include <android/log.h> // На всякий случай




#undef LOG_TAG
#define LOG_TAG "SoLoader"

// Прямые макросы, чтобы не зависеть от чужих файлов
#define LOG_D(fmt, ...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "[%d|%ld] " fmt, getpid(), syscall(SYS_gettid), ##__VA_ARGS__)
#define LOG_E(fmt, ...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "[%d|%ld] " fmt, getpid(), syscall(SYS_gettid), ##__VA_ARGS__)
#define LOG_W(fmt, ...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "[%d|%ld] " fmt, getpid(), syscall(SYS_gettid), ##__VA_ARGS__)
#define LOG_I(fmt, ...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, "[%d|%ld] " fmt, getpid(), syscall(SYS_gettid), ##__VA_ARGS__)
#define TO_STR(view) std::string(view)

using namespace jni;


JavaVM* g_vm_global = nullptr;
static std::unique_ptr<jni::JvmRef<jni::kDefaultJvm>> g_jvm;
std::string GLOBAL_CACHE_DIR = "";
std::string GLOBAL_PKG_NAME = "";
const std::string SD_ROOT = "/storage/emulated/0/Documents/SoLoader";

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
                                       Field{"mBoundApplication", kAppBindData}
};
static constexpr Class kSystem{"java/lang/System",
                               Static {
                                       Method{"load", Return<void>{}, Params<jstring>{}}
                               }
};



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
    std::stringstream ss(path);
    std::string item, current_path = "";
    while (std::getline(ss, item, '/')) {
        if (item.empty()) { current_path += "/"; continue; }
        current_path += item + "/";
        mkdir(current_path.c_str(), 0777);
    }
}
// Глобальная переменная для пути (нужна обработчику сигналов)


void fast_clear_all_cache(const std::string& cachePath) {
    // В обработчике сигналов нельзя использовать тяжелые функции,
    // но remove() и opendir() обычно отрабатывают нормально.
    DIR* dir = opendir(cachePath.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            remove((cachePath + "/" + entry->d_name).c_str());
        }
        closedir(dir);
    }

    // После очистки — выходим штатно, чтобы не блокировать закрытие

}

// Вызови это один раз в JNI_OnLoad



void cleanup_old_cache(const std::string& cachePath) {
    if (cachePath.empty()) return;

    DIR* dir = opendir(cachePath.c_str());
    if (!dir) return;

    struct dirent* entry;
    time_t now = time(nullptr);
    // 24 часа. Если хочешь быстрее — ставь 3600 (1 час)
    const long MAX_AGE = 1 * 60 * 60;

    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;

        // Нас интересуют только либы
        if (name.find(".so") != std::string::npos) {
            std::string fullPath = cachePath + "/" + name;
            struct stat s;

            if (stat(fullPath.c_str(), &s) == 0) {
                // Если разница во времени больше MAX_AGE — удаляем
                if (difftime(now, s.st_mtime) > MAX_AGE) {
                    if (remove(fullPath.c_str()) == 0) {
                        LOG_D("Cleanup: deleted old file %s", name.c_str());
                    }
                }
            }
        }
    }
    closedir(dir);
}

bool copyFile(const std::string& src, const std::string& dst) {
    std::ifstream source(src, std::ios::binary);
    std::ofstream dest(dst, std::ios::binary);
    if (!source.is_open() || !dest.is_open()) return false;
    dest << source.rdbuf();
    chmod(dst.c_str(), 0755);
    return true;
}




void init_virtual_paths(JNIEnv* env) {
    int retry = 0;
    LOG_D(" >>> Entering init_virtual_paths");
    while (retry < 300) {
        LOG_D(" [Attempt %d] Searching for ActivityThread...", retry);
        // 1. Пытаемся получить доступ к ActivityThread
        auto activityThread = jni::StaticRef<kActivityThread>{};
        auto appJob = activityThread("currentApplication");
        LOG_D(" [Attempt %d] currentApplication call finished", retry);
        if (static_cast<jobject>(appJob) != nullptr) {
            LOG_D(" [Attempt %d] SUCCESS: appJob found!", retry);
            if (g_jvm) {
                g_jvm->SetFallbackClassLoaderFromJObject(static_cast<jobject>(appJob));
            } else {
                LOGE("[SoLoader] CRITICAL: g_jvm is NULL, skipping FallbackLoader!");
            }
            jni::LocalObject<kContext> app{std::move(appJob)};
            LOG_D(" LocalObject<kContext> initialized");
            LOG_D(" Requesting getPackageName...");
            auto pkgName = app("getPackageName");
            if (static_cast<jstring>(pkgName) != nullptr) {
                std::string tmpName = TO_STR(pkgName.Pin().ToString());
                if (GLOBAL_PKG_NAME.empty() || (tmpName.find("com.gspace.android") == std::string::npos)) {
                    GLOBAL_PKG_NAME = tmpName;
                }
                LOG_D(" GLOBAL_PKG_NAME set to: %s", GLOBAL_PKG_NAME.c_str());
            }
            LOG_D(" Requesting getCacheDir...");
            auto cacheFileObj = app("getCacheDir");
            if (static_cast<jobject>(cacheFileObj) != nullptr) {
                LOG_D("cacheFileObj obtained, converting to LocalObject...");
                jni::LocalObject<kFile> cacheFile{std::move(cacheFileObj)};
                auto pathString = cacheFile("getAbsolutePath");
                if (static_cast<jstring>(pathString) != nullptr) {
                    std::string tmpCache = TO_STR(pathString.Pin().ToString());
                    if (GLOBAL_CACHE_DIR.empty() || (tmpCache.find("/virtual/") != std::string::npos)) {
                        GLOBAL_CACHE_DIR = tmpCache;
                    }
                    LOG_D(" GLOBAL_CACHE_DIR set to: %s", GLOBAL_CACHE_DIR.c_str());
                }
            }
            LOG_D(" Checking mBoundApplication for real process name...");
            auto atObj = activityThread("currentActivityThread");
            if (static_cast<jobject>(atObj) != nullptr) {
                jni::LocalObject<kActivityThread> at{std::move(atObj)};
                auto bindDataRaw = at.Access<"mBoundApplication">().Get();
                if (static_cast<jobject>(bindDataRaw) != nullptr) {
                    jni::LocalObject<kAppBindData> bindData{std::move(bindDataRaw)};
                    auto procNameJS = bindData.Access<"processName">().Get();
                    if (static_cast<jstring>(procNameJS) != nullptr) {
                        std::string realName = TO_STR(procNameJS.Pin().ToString());
                        LOG_D(" Real Process Name: %s", realName.c_str());
                        if (!realName.empty() && realName.find("com.gspace.android") == std::string::npos) {
                            GLOBAL_PKG_NAME = realName;
                            LOG_D(" !!! TARGET CAUGHT IN BIND DATA !!! PKG: %s", GLOBAL_PKG_NAME.c_str());
                        }
                    }
                }
            }
            /*
            LOG_D(" Checking ClassLoader for virtual environment...");
            auto curThreadJob = jni::StaticRef<kThread>{}("currentThread");
            if (static_cast<jobject>(curThreadJob) != nullptr) {
                LOG_D(" currentThread found, getting ClassLoader...");
                jni::LocalObject<kThread> curThread{std::move(curThreadJob)};
                auto loaderJob = curThread("getContextClassLoader");
                if (static_cast<jobject>(loaderJob) != nullptr) {
                    LOG_D(" ClassLoader found, calling toString...");
                    jni::LocalObject<kClassLoader> loader{std::move(loaderJob)};
                    std::string loaderInfo { std::string(loader("toString").Pin().ToString()) };
                    LOG_D(" Loader String: %s", loaderInfo.c_str());
                    size_t pPos = loaderInfo.find("permitted_path=");
                    if (pPos != std::string::npos) {
                        LOG_D(" 'permitted_path' keyword detected, parsing...");
                        std::string pPath = loaderInfo.substr(pPos + 15);
                        size_t end = pPath.find_first_of(", \n]");
                        if (end != std::string::npos) pPath = pPath.substr(0, end);
                        size_t lastColon = pPath.find_last_of(':');
                        std::string targetPath = (lastColon != std::string::npos) ? pPath.substr(lastColon + 1) : pPath;
                        LOG_D(" Extracted target path: %s", targetPath.c_str());
                        if (targetPath.find("/virtual/") != std::string::npos) {
                            LOG_D(" GSpace Virtual Path detected!");
                            GLOBAL_PKG_NAME = targetPath.substr(targetPath.find_last_of('/') + 1);
                            GLOBAL_CACHE_DIR = targetPath + "/cache";
                            LOG_D(" !!! GSpace OVERRIDE !!! PKG: %s | Cache: %s",GLOBAL_PKG_NAME.c_str(), GLOBAL_CACHE_DIR.c_str());
                            return;
                        }
                    }
                }
            }
            */
            if (!GLOBAL_PKG_NAME.empty() && GLOBAL_PKG_NAME.find("com.gspace.android") == std::string::npos) {
                LOG_D(" <<< SUCCESS: Guest app identified: %s", GLOBAL_PKG_NAME.c_str());
                return;
            }
        } else {
            LOGW("[SoLoader] appJob is NULL, ActivityThread not ready.");
        }

        usleep(250000);
        retry++;
    }
    LOGE("[SoLoader] !!! FAILED to init virtual paths after 100 retries !!!");
}


std::string getCleanName(const std::string& name) {
    size_t wlPos = name.find("SLWL_");
    if (wlPos != std::string::npos) return name.substr(0, wlPos);

    size_t loadPos = name.find("SLLoad.so");
    if (loadPos != std::string::npos) return name.substr(0, loadPos);

    return name;
}

void LoadAndCleanupLibrary(const std::string& currentPath, const std::string& fileName, bool isExternal) {
    std::string directory = currentPath.substr(0, currentPath.find_last_of('/') + 1);
    std::string cleanName = getCleanName(fileName);
    std::string targetPath = directory + cleanName;

    // ЛОГИКА: Если external — RENAME, если нет — COPY
    bool ready = false;
    if (isExternal) {
        ready = (rename(currentPath.c_str(), targetPath.c_str()) == 0);
    } else {
        ready = true;
    }

    if (ready) {
        // 1. Проверка на конфиг (просто удаляем после паузы)
        if (fileName.find("config") != std::string::npos || fileName.find("json") != std::string::npos) {
            LOG_D("Config processed: %s", cleanName.c_str());
            //remove(targetPath.c_str()); не трогаем конфиги их удалит система со временем!
            return;
        }

        // 2. Загрузка библиотеки
        void* h = dlopen(targetPath.c_str(), RTLD_NOW);
        if (h) {
            LOG_D("Successfully Loaded: %s", cleanName.c_str());
            typedef jint (*JNI_OnLoad_t)(JavaVM*, void*);
            auto pJNI_OnLoad = (JNI_OnLoad_t)dlsym(h, "JNI_OnLoad");

            if (pJNI_OnLoad && g_vm_global) {
                JNIEnv* env = nullptr;
                bool threadWasAttachedByUs = false;

                // Проверяем: приаттачен ли текущий поток (например, если вызвано из слушателя)
                jint envRes = g_vm_global->GetEnv((void**)&env, JNI_VERSION_1_6);

                if (envRes == JNI_EDETACHED) {
                    // Поток не приаттачен (слушатель или воркер) — аттачим!
                    if (g_vm_global->AttachCurrentThread(&env, nullptr) == JNI_OK) {
                        threadWasAttachedByUs = true;
                        LOG_D("Thread attached for loading: %s", cleanName.c_str());
                    }
                }

                // Теперь, когда env точно есть, вызываем инициализатор мода
                if (env) {
                    LOG_D("Calling JNI_OnLoad for %s", cleanName.c_str());
                    pJNI_OnLoad(g_vm_global, nullptr);
                }

                // Отсоединяем ТОЛЬКО если мы сами его приаттачили в этой функции
                if (threadWasAttachedByUs) {
                    g_vm_global->DetachCurrentThread();
                    LOG_D("Thread detached after loading");
                }
            }




            //RemapTools::RemapLibrary(cleanName.c_str());
        } else {
            LOG_E("Load Error: %s", dlerror());
        }

        if (isExternal) {
            // 3. Чистка
            // Создаем отдельный поток специально для удаления
            std::thread([targetPath, cleanName]() {
                sleep(10); // Ждем достаточно долго
                if (remove(targetPath.c_str()) == 0) {
                    LOG_D("Delayed cleanup SUCCESS: %s", cleanName.c_str());
                } else {
                    LOG_E("Delayed cleanup FAILED: %s (errno: %d)", cleanName.c_str(), errno);
                }
            }).detach(); // Отсоединяем, чтобы он жил сам по себе
        }
    }
}


void waitAndLoadWorker(std::string fullPath, std::string targetLib, std::string fileName, bool isExternal) {
    LOG_D(" Thread started: waiting for %s to load %s", targetLib.c_str(), fileName.c_str());
    int timeout = 800;
    int elapsed = 0;
    bool load = true;
    while (!isLibraryLoaded(targetLib.c_str())) {
        if (elapsed >= timeout) {
            LOG_D("waitAndLoadWorker: Timeout reached: %s not found.", targetLib.c_str());
            load = false;
            break;
        }
        usleep(500000);
        elapsed++;
    }
    if(load){
        LOG_D(" Target %s detected! Loading %s now...", targetLib.c_str(), fileName.c_str());
        LoadAndCleanupLibrary(fullPath, fileName, isExternal);
    }
}
void processAndLoad(std::string fullPath, std::string fileName, bool isExternal) {
    // В этой версии мы не делаем предварительных манипуляций,
    // всё делегируем в LoadAndCleanupLibrary или Worker

    std::string finalPath = fullPath;
    std::string uniqueName = fileName;

    if (isExternal) {
        if (GLOBAL_CACHE_DIR.empty()) return;

        // Если это не библиотека с ожиданием (SLWL_), просто клеим Load.so в конец
        if (uniqueName.find("SLWL_") == std::string::npos &&
            uniqueName.find("SLLoad.so") == std::string::npos) {

            uniqueName += "SLLoad.so";
            // Результат: libmod.so -> libmod.soSLLoad.so
        }

        finalPath = GLOBAL_CACHE_DIR + "/" + uniqueName;

        if (!copyFile(fullPath, finalPath)) return;
        LOG_D("Unique instance created: %s", uniqueName.c_str());
    }
    size_t wlPos = uniqueName.find("SLWL_");
    if (wlPos != std::string::npos) {
        // ИСПРАВЛЕНО: Смещение +5 для "SLWL_"
        std::string targetLib = uniqueName.substr(wlPos + 5);
        size_t soPos = targetLib.find(".so");
        if (soPos != std::string::npos) targetLib = targetLib.substr(0, soPos + 3);

        LOG_D("WaitAndLoad detected. Target: %s", targetLib.c_str());
        std::thread(waitAndLoadWorker, finalPath, targetLib, uniqueName, isExternal).detach();
    }
    else if (uniqueName.find("SLLoad.so") != std::string::npos) {
        LoadAndCleanupLibrary(finalPath, uniqueName, isExternal);
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

void start_pid_bridge_listener() {
    if (GLOBAL_CACHE_DIR.empty()) {
        LOG_E("start_pid_bridge_listener: GLOBAL_CACHE_DIR is empty");
        return;
    }

    pid_t myPid = getpid();
    // Имя файла строго под конкретный PID: /storage/emulated/0/Documents/1234_bridge
    std::string bridgePath = "/storage/emulated/0/Documents/" + std::to_string(myPid) + "_bridge";

    LOG_D("Bridge: Listener active for PID: %d", myPid);

    while (true) {
        struct stat st;
        // Проверяем: появился ли файл от ГГ?
        if (stat(bridgePath.c_str(), &st) == 0) {
            LOG_D("Bridge: Task file detected! Reading...");
            std::ifstream file(bridgePath);
            std::string task;
            if (std::getline(file, task)) {
                file.close();
                // СРАЗУ УДАЛЯЕМ, чтобы не прочитать дважды и не плодить мусор
                unlink(bridgePath.c_str());
                LOG_D("Bridge: Task received: %s", task.c_str());
                // Парсим формат: ПУТЬ_К_ЛИБЕ|ФУНКЦИЯ
                size_t pipe = task.find('|');
                std::string libPath = (pipe != std::string::npos) ? task.substr(0, pipe) : task;
                std::string funcName = (pipe != std::string::npos) ? task.substr(pipe + 1) : "";
                if (!libPath.empty()) {
                    // Используем твой уникальный метод загрузки (с копированием в кэш и PID_TID)
                    std::string fileName = libPath.substr(libPath.find_last_of('/') + 1);
                    processAndLoad(libPath, fileName, true);
                    // Если ГГ передал имя функции — дергаем её через dlsym
                    /*
                    if (!funcName.empty()) {
                        // nullptr в dlopen ищет во всех уже загруженных либах процесса
                        void* handle = dlopen(nullptr, RTLD_NOW);
                        auto targetFunc = (void(*)())dlsym(handle, funcName.c_str());

                        if (targetFunc) {
                            targetFunc();
                            LOG_D("Bridge: Function %s executed successfully!", funcName.c_str());
                        } else {
                            LOG_E("Bridge: Function %s not found in loaded modules", funcName.c_str());
                        }
                    }
                    */
                }
            } else {
                file.close();
                unlink(bridgePath.c_str());
            }
        }
        sleep(5);
    }
}

#define libTarget "libil2cpp.so"

void dump_thread() {
    JNIEnv* env;
    if (g_vm_global->AttachCurrentThread(&env, nullptr) != JNI_OK)
        return;
    // ВОТ ЭТА СТРОКА: Инициализирует JniBind для этого потока
    LOG_D(" jni::ThreadGuard guard");
    jni::ThreadGuard guard;
    LOG_D("preload apk lib");
    loadExtraLibraries();
    LOG_D(" Start initialize process");
    init_virtual_paths(env);
    LOG_D(" Start initialize process finished");
    LOG_D("loadExtraLibraries via virtual path");
    cleanup_old_cache(GLOBAL_CACHE_DIR);

    std::thread(start_pid_bridge_listener).detach();
    loadExtraLibraries();

    bool dump = true;
    int timeout = 120;
    int elapsed = 0;

    while (!isLibraryLoaded(libTarget)) {
        if (elapsed >= timeout) {
            LOG_D(" Timeout reached: %s not found.", libTarget);
            dump = false;
            break;
        }
        sleep(1);
        elapsed++;
    }

    if (dump) {
        LOG_D(" %s detected. Sleeping before dump...", libTarget);
        sleep(Sleep);
        void* il2cpp_handle = dlopen(libTarget, RTLD_NOW);
        if (il2cpp_handle) {
            std::string androidDataPath ="/storage/emulated/0/Documents/" + GLOBAL_PKG_NAME + "-dump.cs";
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

    LOG_D(" JNI System Initialized (Release 1.5.0)");

    std::thread(dump_thread).detach();

    return JNI_VERSION_1_6;
}
