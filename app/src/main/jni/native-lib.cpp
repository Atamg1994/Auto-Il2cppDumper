#include <jni.h>
#include <string>
#include <thread>
#include <dlfcn.h>
#include <unistd.h>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <fstream>

// --- FRIDA GUMJS INCLUDES ---
#include "frida/include/frida-gumjs.h"

#include "Il2Cpp/il2cpp_dump.h"
#include "Includes/config.h"
#include "Includes/log.h"

// Глобальные переменные для Frida
static GumScriptBackend *snity_backend = nullptr;
static GumScript *snity_script = nullptr;
static long last_js_mtime = 0;
static std::string global_script_path = "";
// Добавь эти переменные в начало
static bool config_on_change_reload = false;
static bool config_on_load_wait = false;
static std::string config_runtime = "qjs"; 



// --- БЛОК ЛОГИКИ FRIDA (SNITY LITE) ---

// Парсер конфига (ищет "path":"..." в текстовом файле)
std::string get_config_value(const std::string& content, const std::string& key) {
    size_t pos = content.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = content.find(":", pos);
    size_t start = content.find("\"", pos) + 1;
    size_t end = content.find("\"", start);
    if (start == 0 || end == std::string::npos) return "";
    return content.substr(start, end - start);
}

// Загрузка/Перезагрузка JS
void reload_snity_js() {
    if (global_script_path.empty()) return;
    struct stat st;
    if (stat(global_script_path.c_str(), &st) != 0) return;
    if (st.st_mtime <= last_js_mtime) return; 
    last_js_mtime = st.st_mtime;

    if (snity_script != nullptr) {
        gum_script_unload_sync(snity_script, NULL);
        g_object_unref(snity_script);
        snity_script = nullptr;
    }

    std::ifstream t(global_script_path);
    if (!t.is_open()) return;
    std::string src((std::istreambuf_iterator<char>(t)), std::istreambuf_iterator<char>());
    
    GError *err = NULL;
    snity_script = gum_script_backend_create_sync(snity_backend, "snity-payload", src.c_str(), NULL, NULL, &err);
    if (snity_script) {
        gum_script_load_sync(snity_script, NULL);
        LOGI("SNITY: Script loaded/reloaded: %s", global_script_path.c_str());
    } else {
        LOGE("SNITY: JS Error: %s", err->message);
        g_error_free(err);
    }
}

// Поток мониторинга конфига и скрипта
void snity_monitor_thread(std::string config_path) {
    LOGI("SNITY: Reading config...");
    
    // 1. ПЕРВИЧНОЕ ЧТЕНИЕ КОНФИГА
    std::ifstream c(config_path);
    if (c.is_open()) {
        std::string content((std::istreambuf_iterator<char>(c)), std::istreambuf_iterator<char>());
        global_script_path = get_config_value(content, "path");
        config_runtime = get_config_value(content, "runtime");
        if (config_runtime.empty()) config_runtime = "qjs";
        
        if (content.find("\"on_change\":\"reload\"") != std::string::npos) config_on_change_reload = true;
        if (content.find("\"on_load\":\"wait\"") != std::string::npos) config_on_load_wait = true;
        c.close();
    }

    gum_init_embedded();
    
    // ВЫБОР ДВИЖКА (v8 или qjs)
    if (config_runtime == "v8") {
        LOGI("SNITY: Using V8 runtime");
        snity_backend = gum_script_backend_obtain_v8();
    } else {
        LOGI("SNITY: Using QuickJS runtime");
        snity_backend = gum_script_backend_obtain_qjs();
    }

    // Первый запуск
    reload_snity_js();

    // Если on_load: wait, здесь можно было бы поставить флаг готовности, 
    // но в нашей структуре мы просто продолжаем.

    // ЦИКЛ ПЕРЕЗАГРУЗКИ (on_change: reload)
    while (config_on_change_reload) {
        reload_snity_js();
        sleep(3); 
    }
}

// Поиск пути к конфигу рядом с текущей .so


std::string find_my_config_path() {
    Dl_info info;
    if (dladdr((void*)find_my_config_path, &info) && info.dli_fname) {
        std::string self_path = info.dli_fname; // Полный путь к нашей либе
        
        // 1. Получаем директорию, где лежит либа
        size_t last_slash = self_path.find_last_of("/");
        if (last_slash == std::string::npos) return "";
        std::string lib_dir = self_path.substr(0, last_slash);
        
        // 2. Получаем имя нашей либы без пути (например, libil2cppdumper.so)
        std::string lib_name = self_path.substr(last_slash + 1);
        
        // 3. Отрезаем расширение .so, чтобы получить чистую базу (libil2cppdumper)
        size_t last_dot = lib_name.find_last_of(".");
        std::string base_name = (last_dot != std::string::npos) ? lib_name.substr(0, last_dot) : lib_name;

        // 4. Ищем в этой папке любой файл, начинающийся на "base_name.unityconfig"
        DIR* dir = opendir(lib_dir.c_str());
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string fname = entry->d_name;
                // Ищем совпадение: libil2cppdumper.unityconfig
                if (fname.find(base_name + ".unityconfig") == 0) {
                    std::string full_cfg_path = lib_dir + "/" + fname;
                    closedir(dir);
                    LOGI("SNITY: Found config: %s", full_cfg_path.c_str());
                    return full_cfg_path;
                }
            }
            closedir(dir);
        }
    }
    return "";
}


// --- ТВОЙ ОРИГИНАЛЬНЫЙ КОД БЕЗ ИЗМЕНЕНИЙ ---

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
    LOGI("Scanning directory for 'Load' libraries: %s", libDir.c_str());
    DIR* dir = opendir(libDir.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strstr(entry->d_name, "Load") && strstr(entry->d_name, ".so")) {
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
    LOGI("Lib loaded - SNITY active");
    
    // --- Инициализация Frida (Snity) ---
    std::string cfg_path = find_my_config_path();
    if (!cfg_path.empty()) {
        std::thread(snity_monitor_thread, cfg_path).detach();
    }

    // --- Оригинальная логика загрузки ---
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
    LOGI("Initialize JNI with Snity Lite");
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
