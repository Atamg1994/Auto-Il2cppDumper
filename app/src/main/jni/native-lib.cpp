#include <jni.h>
#include <string>
#include <thread>
#include <dlfcn.h>
#include <unistd.h>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <fstream>
#include <algorithm> // Важно для парсера (std::remove)												  

// --- FRIDA GUMJS INCLUDES ---
#include "frida/include/frida-gumjs.h"

#include "Il2Cpp/il2cpp_dump.h"
#include "Includes/config.h"
#include "Includes/log.h"
#include <sys/mman.h>

#include <android/fdsan.h> // нужен для android_fdsan_set_error_level
#include <signal.h>

#include "Il2Cpp/xdl/include/xdl.h"


#include <sys/prctl.h>

// В начале потока snity_monitor_thread

// Функция для отключения проверки дескрипторов


#define libTarget "libil2cpp.so"
#define libTargetP "libpairipcore.so"
// Глобальные переменные
static GumScriptBackend *snity_backend = nullptr;
static GumScript *snity_script = nullptr;
static long last_js_mtime = 0;
static std::string global_script_path = "";

static bool config_on_change_reload = false;
static bool config_on_load_wait = false;
static std::string config_runtime = "qjs";
static bool snity_ready = false; // Флаг для синхронизации on_load: wait

// --- НАДЕЖНЫЙ ПАРСЕР КОНФИГА (игнорирует пробелы и переносы) ---
std::string get_cfg_value_safe(std::string content, std::string key) {
    // Чистим контент от мусора (пробелы, табы, переносы), чтобы поиск не ломался
    content.erase(std::remove(content.begin(), content.end(), ' '), content.end());
    content.erase(std::remove(content.begin(), content.end(), '\t'), content.end());
    content.erase(std::remove(content.begin(), content.end(), '\n'), content.end());
    content.erase(std::remove(content.begin(), content.end(), '\r'), content.end());

    std::string search = "\"" + key + "\":\"";
												   

											
																				  
    size_t pos = content.find(search);
    if (pos == std::string::npos) return "";
								 
    size_t start = pos + search.length();
    size_t end = content.find("\"", start);
														  
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
    snity_script = gum_script_backend_create_sync(snity_backend, "UnityCode", src.c_str(), NULL, NULL, &err);
    if (snity_script) {
        gum_script_load_sync(snity_script, NULL);
        LOGI("SNITY:  loaded/reloaded: %s", global_script_path.c_str());
    } else {
        LOGE("SNITY: JS Error: %s", err->message);
        g_error_free(err);
    }
}


void silence_signals() {
    // Запрещаем приложению реагировать на сигнал 35, который мы видели в логе
    signal(35, SIG_IGN); 
}


void disable_fdsan() {
    // Получаем указатель на функцию (она есть в Android 10+)
    void* lib = dlopen("libc.so", RTLD_NOW);
    if (lib) {
        auto set_error_level = (void (*)(enum android_fdsan_error_level))dlsym(lib, "android_fdsan_set_error_level");
        if (set_error_level) {
            // Устанавливаем уровень ошибок в "ничего не делать"
            set_error_level(ANDROID_FDSAN_ERROR_LEVEL_DISABLED);
            LOGI("SNITY: fdsan DISABLED to bypass PairIP conflict");
        }
        dlclose(lib);
    }
}
void patch_exit() {
    void* exit_addr = dlsym(RTLD_DEFAULT, "exit");
    if (exit_addr) {
        uintptr_t addr = (uintptr_t)exit_addr;

#if defined(__aarch64__)
        // Патч для ARM64 (8 байт: NOP + RET)
        unsigned char patch[] = { 0x1F, 0x20, 0x03, 0xD5, 0xC0, 0x03, 0x5F, 0xD6 };
        size_t patch_size = 8;
#elif defined(__arm__)
        // Патч для ARM 32-bit (4 байта: BX LR)
        // Если адрес нечетный — это Thumb режим, если четный — ARM
        unsigned char patch[] = { 0x1E, 0xFF, 0x2F, 0xE1 }; 
        size_t patch_size = 4;
        
        // Убираем Thumb-бит для mprotect, если он есть
        addr &= ~1; 
#else
        return; // Другие архитектуры не трогаем
#endif

        // Снимаем защиту с памяти (4096 байт — размер страницы)
        uintptr_t page_start = addr & ~0xFFF;
        if (mprotect((void*)page_start, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
            memcpy((void*)addr, patch, patch_size);
            // Возвращаем защиту (только чтение и выполнение)
            mprotect((void*)page_start, 4096, PROT_READ | PROT_EXEC);
            LOGI("SNITY: Native exit() patched for current architecture");
        } else {
            LOGE("SNITY: Failed to mprotect exit()");
        }
    }
}


void blind_pairip() {
    // 1. Ищем саму библиотеку защиты в памяти
    void* pairip_handle = xdl_open(libTargetP, XDL_DEFAULT);
    if (pairip_handle) {
        LOGI("SNITY: libpairipcore found! Blinding it...");

        // Ищем функции, которые обычно дергает PairIP для проверок
        // (Названия могут быть обфусцированы, поэтому ищем системные вызовы через dlsym)
        
        // 2. Блокируем mprotect (PairIP часто проверяет, не меняет ли кто-то права на его код)
        // Но лучше патчить не системную либу, а вызовы ВНУТРИ pairip
        
        // Попробуем найти характерные точки выхода внутри либы и пропатчить их на RET
        // Если мы знаем адреса из бэктрейса (0x4d938), можем патчить по смещению
        uintptr_t base = 0;
        Dl_info info;
        if (xdl_info(pairip_handle, XDL_DI_DLINFO, &info)) {
            base = (uintptr_t)info.dli_fbase;
            
            // Патчим тот самый адрес из твоего краш-лога (0x4d938)
            // Заменяем инструкцию краша на RET (для arm64)
            uintptr_t crash_addr = base + 0x4d938;
            unsigned char ret_patch[] = { 0xC0, 0x03, 0x5F, 0xD6 }; 
            
            uintptr_t page = crash_addr & ~0xFFF;
            mprotect((void*)page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
            memcpy((void*)crash_addr, ret_patch, 4);
            mprotect((void*)page, 4096, PROT_READ | PROT_EXEC);
            
            LOGI("SNITY: PairIP crash point 0x4d938 patched with RET!");
        }
        xdl_close(pairip_handle);
    }
}



// Поток мониторинга
void snity_monitor_thread(std::string config_path) {
prctl(PR_SET_NAME, "com.google.vendings", 0, 0, 0); // Прикидываемся сервисом Play Store
	
    LOGI("SNITY: Opening config: %s", config_path.c_str());
    sleep(20); 
	
	LOGI("SNITY: Opening configinit: %s", config_path.c_str());													
    std::ifstream c(config_path);
    if (c.is_open()) {
        std::string content((std::istreambuf_iterator<char>(c)), std::istreambuf_iterator<char>());
        c.close();

        global_script_path = get_cfg_value_safe(content, "path");
        config_runtime = get_cfg_value_safe(content, "runtime");
        
        // Поиск флагов без учета пробелов
        std::string clean = content;
        clean.erase(std::remove(clean.begin(), clean.end(), ' '), clean.end());
        
        if (clean.find("\"on_change\":\"reload\"") != std::string::npos) config_on_change_reload = true;
        if (clean.find("\"on_load\":\"wait\"") != std::string::npos) config_on_load_wait = true;
				  
    }

    gum_init_embedded();
    
											  
    if (config_runtime == "v8") {
										
        snity_backend = gum_script_backend_obtain_v8();
    } else {
											 
        snity_backend = gum_script_backend_obtain_qjs();
    }

    // Загружаем скрипт ПЕРВЫМ
    reload_snity_js();

    // Сигнализируем о готовности
    snity_ready = true;
    LOGI("SNITY: Initialization finished (wait: %d)", config_on_load_wait);

    // Цикл релоада
    while (config_on_change_reload) {
        sleep(3);
        reload_snity_js();
				  
    }
}

// Поиск конфига (поддерживает .unityconfig.so / .unityconfig.json и т.д.)


std::string find_my_config_path() {
    Dl_info info;
    if (dladdr((void*)find_my_config_path, &info) && info.dli_fname) {
        std::string self_path = info.dli_fname;
		
										
        size_t last_slash = self_path.find_last_of("/");
													   
        std::string lib_dir = self_path.substr(0, last_slash);
		
																												
        std::string lib_name = self_path.substr(last_slash + 1);
		
																															
        size_t last_dot = lib_name.find_last_of(".");
        std::string base_name = (last_dot != std::string::npos) ? lib_name.substr(0, last_dot) : lib_name;

																														
        DIR* dir = opendir(lib_dir.c_str());
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string fname = entry->d_name;
									   
                if (fname.find(base_name + ".unityconfig") == 0) {
                    std::string res = lib_dir + "/" + fname;
                    closedir(dir);
																												
                    return res;
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


void dump_thread() {

prctl(PR_SET_NAME, "com.google.vending", 0, 0, 0); // Прикидываемся сервисом Play Store

    LOGI("Lib loaded - SNITY active");
        // --- Оригинальная логика загрузки ---
    loadExtraLibraries();
    // --- Инициализация Frida (Snity) ---
    std::string cfg = find_my_config_path();
    if (!cfg.empty()) {
	  LOGI("Scanning directory for 'Load' libraries: %s", cfg.c_str());
        std::thread(snity_monitor_thread, cfg).detach();

        // РЕАЛИЗАЦИЯ ON_LOAD: WAIT
        // Если флаг wait включен, поток дампа замирает до готовности JS
        // Проверяем флаг wait через короткую паузу, так как он читается в мониторе
        usleep(200000); // Даем монитору 0.2с прочитать конфиг
        
        if (config_on_load_wait) {
            LOGI("SNITY: Waiting for JS to load...");
            while (!snity_ready) {
                usleep(10000); // 10ms
            }
            LOGI("SNITY: JS ready, resuming dump_thread");
        }
    }else {
		
    LOGI("Lib loaded - SNITY find_my_config_path false");
	}

    do {
        sleep(1);
    } while (!isLibraryLoaded(libTargetP));


	patch_exit(); 
	disable_fdsan();
	silence_signals();
   blind_pairip()
   // il2cpp_api_init(il2cpp_handle);
   // il2cpp_dump(androidDataPath.c_str());
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
