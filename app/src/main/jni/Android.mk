LOCAL_PATH := $(call my-dir)
MAIN_LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE    := ModLoader

# Добавили -D_GNU_SOURCE для работы RTLD_NEXT и хуков
LOCAL_CFLAGS := -Wno-error=format-security -fvisibility=hidden \
                -ffunction-sections -fdata-sections -w -fpermissive \
                -D_GNU_SOURCE

# Добавили флаги для C++20 и поддержки JNI-BIND
LOCAL_CPPFLAGS := -std=c++20 -frtti -fexceptions \
                  -Wno-error=format-security -fvisibility=hidden \
                  -ffunction-sections -fdata-sections -w -fpermissive \
                  -frelaxed-template-template-args \
                  -DJNI_BIND_MAX_ARG_COUNT=10 \
                  -D_GNU_SOURCE

# Оптимизация размера и удаление лишних секций
LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-all

LOCAL_ARM_MODE := arm

# Пути к инклудам
LOCAL_C_INCLUDES += $(MAIN_LOCAL_PATH) \
                    $(LOCAL_PATH) \
                    $(LOCAL_PATH)/Includes \
                    $(LOCAL_PATH)/Il2Cpp/xdl/include

# Автоматический поиск всех .c файлов в папке xdl
FILE_LIST := $(wildcard $(LOCAL_PATH)/Il2Cpp/xdl/*.c)

# Формируем список исходников
LOCAL_SRC_FILES := $(FILE_LIST:$(LOCAL_PATH)/%=%) \
                   native-lib.cpp \
                   Il2Cpp/il2cpp_dump.cpp

# Библиотеки для линковки
LOCAL_LDLIBS := -llog -landroid -lGLESv2 -ldl

include $(BUILD_SHARED_LIBRARY)
