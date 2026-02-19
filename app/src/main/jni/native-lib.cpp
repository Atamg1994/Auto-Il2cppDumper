#include <jni.h>
#include <string>
#include <thread>
#include <dlfcn.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include "Il2Cpp/il2cpp_dump.h"
#include "Includes/config.h"
#include "Includes/log.h"
#include "Includes/jni_bind_release.h"

using namespace jni;

// --- Глобальные данные ---
JavaVM* g_vm_global = nullptr;
std::string GLOBAL_CACHE_DIR = "";
std::string GLOBAL_PKG_NAME = "";
const std::string SD_ROOT = "/storage/emulated/0/Documents/SoLoader";

// --- Описание Java классов для JNI-Bind ---
// --- Описание Java классов (Исправленный синтаксис JNI-Bind) ---
static constexpr Class kFile{"java/io/File", 
    Method{"getAbsolutePath", Return<jstring>{}}
};
static constexpr Class kContext{"android/content/Context", 
    Method{"getCacheDir", Return{kFile}}, 
    Method{"getPackageName", Return<jstring>{}}
};
static constexpr Class kActivityThread{"android/app/ActivityThread", 
    Method{"currentApplication", jni::Static, Return{"android/app/Application"}}
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

// Получение путей через JNI-Bind (специально для виртуалки)
void init_virtual_paths(JNIEnv* env) {
    JvmRef<kDefaultJvm> jvm{g_vm_global};
    LocalContext ctx{env};
    
    auto app = Class<kActivityThread>::CallStatic("currentApplication");
    int retry = 0;
    while (!app && retry < 50) { // Ожидание инициализации Application в GSpace
        usleep(200000);
        app = Class<kActivityThread>::CallStatic("currentApplication");
        retry++;
    }

    if (app) {
        GLOBAL_PKG_NAME = app.Call(kContext, "getPackageName");
        LocalObject<kFile> cacheFile = app.Call(kContext, "getCacheDir");
        GLOBAL_CACHE_DIR = cacheFile.Call(kFile, "getAbsolutePath");
        LOGI("[SoLoader] Virtual Package: %s", GLOBAL_PKG_NAME.c_str());
        LOGI("[SoLoader] Virtual Cache: %s", GLOBAL_CACHE_DIR.c_str());
    }
}

// Универсальная загрузка (как изнутри, так и снаружи)
void processAndLoad(std::string fullPath, std::string fileName, bool isExternal) {
    std::string finalPath = fullPath;
    
    if (isExternal) {
        finalPath = GLOBAL_CACHE_DIR + "/" + fileName;
        if (!copyFile(fullPath, finalPath)) {
            LOGE("Copy failed: %s", fileName.c_str());
            return;
        }
    }

    if (fileName.find("libWL_") == 0) {
        size_t first = fileName.find("_"), second = fileName.find("_", first + 1);
        if (first != std::string::npos && second != std::string::npos) {
            std::string target = fileName.substr(first + 1, second - first - 1);
            std::thread(waitAndLoadWorker, finalPath, target, fileName).detach();
        }
    } else if (fileName.find("Load") != std::string::npos) {
        void* h = dlopen(finalPath.c_str(), RTLD_NOW);
        if (h) LOGI("Loaded: %s", fileName.c_str());
        else LOGE("Load Error %s: %s", fileName.c_str(), dlerror());
    }
}

void loadExtraLibraries() {
    // 1. Внутреннее сканирование (как раньше)
    char line[512]; std::string libDir = "";
    FILE *fp = fopen("/proc/self/maps", "rt");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, "/lib/") && strstr(line, ".so")) {
                std::string path = line;
                size_t s = path.find("/"), e = path.find_last_of("/");
                libDir = path.substr(s, e - s); break;
            }
        }
        fclose(fp);
    }

    if (!libDir.empty()) {
        DIR* dir = opendir(libDir.c_str());
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (strstr(entry->d_name, "native-lib") || !strstr(entry->d_name, ".so")) continue;
                processAndLoad(libDir + "/" + entry->d_name, entry->d_name, false);
            }
            closedir(dir);
        }
    }

    // 2. Внешнее сканирование (SDCard -> Cache -> Load)
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
		LOGI("[SoLoader] AttachCurrentThread.");
    g_vm_global->AttachCurrentThread(&env, nullptr);
		LOGI("[SoLoader] AttachCurrentThread ok.");
    
    init_virtual_paths(env);
    loadExtraLibraries();
	bool dump = true;
    int timeout = 120; // Лимит в секундах
    int elapsed = 0;
    do {
          if (elapsed >= timeout) {
                LOGI("[SoLoader] Timeout reached: %s not found. Killing thread.", libTarget);
                dump = false;
				break;
          }
          sleep(1);
          elapsed++;
    } while (!isLibraryLoaded(libTarget));
	
    sleep(Sleep);
    if (dump) {
    void* il2cpp_handle = dlopen(libTarget, RTLD_NOW);
		if (il2cpp_handle) {
			std::string androidDataPath = "/storage/emulated/0/Documents/" + GLOBAL_PKG_NAME + "-dump.cs";
			LOGI("[SoLoader] Start dumping: %s:%s", GLOBAL_PKG_NAME.c_str(),androidDataPath.c_str());
			if(il2cpp_api_init(il2cpp_handle)){
			il2cpp_dump(androidDataPath.c_str());
			}
		}
	}
	LOGI("[SoLoader] DetachCurrentThread.");
	    init_virtual_paths(env);
	LOGI("[SoLoader] DetachCurrentThread.");
    g_vm_global->DetachCurrentThread();
}

// --- JNI Вход ---
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    g_vm_global = vm;
    LOGI("[SoLoader] JNI System Initialized");
    std::thread(dump_thread).detach();
    return JNI_VERSION_1_6;
}


