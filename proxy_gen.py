import lief
import os

# Твои два исходника
targets = {
    "arm64": "libkxqpplatform.so",
    "arm32": "libkxqpplatform_32.so"
}
proxy_cpp = "proxy_final.cpp"

def generate():
    all_symbols = set()
    
    # Собираем имена функций из обоих файлов
    for arch, path in targets.items():
        if os.path.exists(path):
            print(f"Парсим {arch}: {path}...")
            lib = lief.parse(path)
            for symbol in lib.exported_symbols:
                name = symbol.name
                if name and not name.startswith('_') and name != 'JNI_OnLoad':
                    all_symbols.add(name)
        else:
            print(f"Внимание: {path} не найден, пропускаем.")

    with open(proxy_cpp, "w") as f:
        f.write('#include <jni.h>\n#include <dlfcn.h>\n#include <android/log.h>\n\n')
        f.write('static void* hOrig = nullptr;\n\n')

        # Конструктор: сам выберет, какой файл грузить
        f.write('__attribute__((constructor)) void init_proxy() {\n')
        f.write('    #if defined(__aarch64__)\n')
        f.write('        hOrig = dlopen("libkxqpplatform_real.so", RTLD_NOW);\n')
        f.write('    #else\n')
        f.write('        hOrig = dlopen("libkxqpplatform_32_real.so", RTLD_NOW);\n')
        f.write('    #endif\n\n')
        f.write('    // === ТВОЙ КОД ЗДЕСЬ ===\n')
        f.write('    __android_log_print(ANDROID_LOG_INFO, "PROXY", "INJECTED SUCCESS");\n')
        f.write('}\n\n')

        # Генерируем обертки для всех найденных имен
        for name in sorted(all_symbols):
            f.write(f'extern "C" void* {name}(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {{\n')
            f.write(f'    typedef void* (*f_t)(void*, void*, void*, void*, void*, void*, void*, void*);\n')
            f.write(f'    static f_t o = nullptr;\n')
            f.write(f'    if (!o) o = (f_t)dlsym(hOrig, "{name}");\n')
            f.write(f'    return o ? o(a1, a2, a3, a4, a5, a6, a7, a8) : nullptr;\n')
            f.write('}\n\n')

    print(f"\nГотово! Сгенерировано {len(all_symbols)} функций в {proxy_cpp}")

generate()
