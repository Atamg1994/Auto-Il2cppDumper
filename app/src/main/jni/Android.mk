LOCAL_PATH := $(call my-dir)
MAIN_LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE    := ModLoader

# Базовые флаги (убрали -fno-rtti и -fno-exceptions)
LOCAL_CFLAGS := -Wno-error=format-security -fvisibility=hidden -ffunction-sections -fdata-sections -w -fpermissive

# ФЛАГИ ДЛЯ ПОДДЕРЖКИ JNI-BIND (ВКЛЮЧАЕМ RTTI И ИСКЛЮЧЕНИЯ)
LOCAL_CPPFLAGS := -std=c++20 -frtti -fexceptions \
                  -Wno-error=format-security -fvisibility=hidden \
                  -ffunction-sections -fdata-sections -w -fpermissive \
                  -frelaxed-template-template-args \
                  -DJNI_BIND_MAX_ARG_COUNT=10

LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-all -llog
LOCAL_ARM_MODE := arm

LOCAL_C_INCLUDES += $(MAIN_LOCAL_PATH) \
                    $(LOCAL_PATH) \
                    $(LOCAL_PATH)/Includes \
                    $(LOCAL_PATH)/Il2Cpp/xdl/include

# Получаем список файлов xdl
FILE_LIST := $(wildcard $(LOCAL_PATH)/Il2Cpp/xdl/*.c)

LOCAL_SRC_FILES := $(FILE_LIST:$(LOCAL_PATH)/%=%) \
                   native-lib.cpp \
                   Il2Cpp/il2cpp_dump.cpp

LOCAL_LDLIBS := -llog -landroid -lGLESv2 -ldl

include $(BUILD_SHARED_LIBRARY)
