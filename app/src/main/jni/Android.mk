LOCAL_PATH := $(call my-dir)
MAIN_LOCAL_PATH := $(call my-dir)

# --- Блок Frida ---
include $(CLEAR_VARS)
LOCAL_MODULE := frida-gumjs
LOCAL_SRC_FILES := frida/lib/$(TARGET_ARCH_ABI)/libfrida-gumjs.a
include $(PREBUILT_STATIC_LIBRARY)
# ------------------

include $(CLEAR_VARS)

LOCAL_MODULE    := il2cppdumper

LOCAL_CFLAGS := -Wno-error=format-security -fvisibility=hidden -ffunction-sections -fdata-sections -w
LOCAL_CFLAGS += -fno-rtti -fno-exceptions -fpermissive
LOCAL_CPPFLAGS := -Wno-error=format-security -fvisibility=hidden -ffunction-sections -fdata-sections -w -Werror -s -std=c++17
LOCAL_CPPFLAGS += -Wno-error=c++11-narrowing -fms-extensions -fno-rtti -fno-exceptions -fpermissive

# Исправлено: убрана лишняя запятая и добавлена маскировка библиотек
LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-all -Wl,--exclude-libs,ALL
LOCAL_ARM_MODE := arm

LOCAL_C_INCLUDES += $(MAIN_LOCAL_PATH)
LOCAL_C_INCLUDES += $(LOCAL_PATH)
LOCAL_C_INCLUDES += $(LOCAL_PATH)/frida/include
LOCAL_C_INCLUDES += $(LOCAL_PATH)/Il2Cpp/xdl/include

FILE_LIST := $(wildcard $(LOCAL_PATH)/Il2Cpp/xdl/*.c)

LOCAL_SRC_FILES := $(FILE_LIST:$(LOCAL_PATH)/%=%)
LOCAL_SRC_FILES += native-lib.cpp \
                   Il2Cpp/il2cpp_dump.cpp

LOCAL_STATIC_LIBRARIES := frida-gumjs
LOCAL_LDLIBS := -llog -landroid -lGLESv2 -ldl -lm

include $(BUILD_SHARED_LIBRARY)
