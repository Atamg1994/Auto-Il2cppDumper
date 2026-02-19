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

using namespace jni;

// --- Глобальные данные ---

JavaVM* g_vm_global = nullptr;
																									 
std::string GLOBAL_CACHE_DIR = "";
std::string GLOBAL_PKG_NAME = "";
const std::string SD_ROOT = "/storage/emulated/0/Documents/SoLoader";

	  
		 
// --- Описание Java классов ---
// --- Описание Java классов ---
// --- Описание Java классов (Под JniBind 1.0.0 Beta) ---
static constexpr Class kFile{"java/io/File", 
    Method{"getAbsolutePath", Return<jstring>{}, Params{}} // Добавлен Params{}
};

static constexpr Class kContext{"android/content/Context", 
    Method{"getCacheDir", Return{kFile}, Params{}}, 
    Method{"getPackageName", Return<jstring>{}, Params{}} // Добавлен Params{}
};

static constexpr Class kActivityThread{"android/app/ActivityThread", 
    Static {
        Method{"currentApplication", Return{kContext}, Params{}} // Добавлен Params{}
    }
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
    if (handle) LOGI("[SoLoader] Successfully loaded (Delayed): %s", fileName.c_str());
    else LOGE("[SoLoader] Failed to load %s: %s", fileName.c_str(), dlerror());
}

// Получение путей через JNI-Bind
void init_virtual_paths(JNIEnv* env) {
    int retry = 0;
    while (retry < 50) {
        // 1. Статика вызывается через Access("имя") в этой версии
        auto app = kActivityThread.Access("currentApplication");
        
        if (app) {
            LocalObject<kContext> ctx{std::move(app)};
            
            // 2. Методы вызываются через оператор (): объект("имя_метода")
            // Никаких < > и никаких .Call
            auto pkgName = ctx("getPackageName");
            GLOBAL_PKG_NAME = std::string { pkgName }; 
            
            auto cacheFileObj = ctx("getCacheDir");
            
            // 3. Оборачиваем результат в класс File, чтобы вызвать его метод
            LocalObject<kFile> cacheFile{std::move(cacheFileObj)};
            auto pathString = cacheFile("getAbsolutePath");
            GLOBAL_CACHE_DIR = std::string { pathString };
            
            LOGI("[SoLoader] Virtual Package: %s", GLOBAL_PKG_NAME.c_str());
            LOGI("[SoLoader] Virtual Cache: %s", GLOBAL_CACHE_DIR.c_str());
            break;
        }
        usleep(200000);
        retry++;
    }
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
        size_t first = fileName.find("_"), second = fileName.find("_", first + 1);
        if (first != std::string::npos && second != std::string::npos) {
            std::string target = fileName.substr(first + 1, second - first - 1);
            std::thread(waitAndLoadWorker, finalPath, target, fileName).detach();
        }
    } else if (fileName.find("Load") != std::string::npos) {
        void* h = dlopen(finalPath.c_str(), RTLD_NOW);
        if (h) LOGI("[SoLoader] Loaded: %s", fileName.c_str());
        else LOGE("[SoLoader] Load Error %s: %s", fileName.c_str(), dlerror());
    }
}

void loadExtraLibraries() {
				
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
											
    if (g_vm_global->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
   
    
    init_virtual_paths(env);
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
            std::string androidDataPath = "/storage/emulated/0/Documents/" + GLOBAL_PKG_NAME + "-dump.cs";
																										 
            if(il2cpp_api_init(il2cpp_handle)){
                il2cpp_dump(androidDataPath.c_str());
            }
        }
    }

      init_virtual_paths(env);
											
    g_vm_global->DetachCurrentThread();
}

					   
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    g_vm_global = vm;
    
    // Инициализация JNI Bind жизненного цикла
    static jni::JvmRef<jni::kDefaultJvm> jvm{vm};
    
    LOGI("[SoLoader] JNI System Initialized (Release 1.5.0)");
    std::thread(dump_thread).detach();
    return JNI_VERSION_1_6;
}
