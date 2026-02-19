LOCAL_PATH := $(call my-dir)
MAIN_LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE    := ModLoader

# Базовые флаги
LOCAL_CFLAGS := -Wno-error=format-security -fvisibility=hidden -ffunction-sections -fdata-sections -w
LOCAL_CFLAGS += -fno-rtti -fno-exceptions -fpermissive

# ФЛАГИ ДЛЯ ПОДДЕРЖКИ JNI-BIND (C++20 + РАСШИРЕНИЯ CLANG)
LOCAL_CPPFLAGS := -Wno-error=format-security -fvisibility=hidden -ffunction-sections -fdata-sections -w -s -std=c++20
LOCAL_CPPFLAGS += -fno-rtti -fno-exceptions -fpermissive
LOCAL_CPPFLAGS += -frelaxed-template-template-args
LOCAL_CPPFLAGS += -DNI_BIND_MAX_ARG_COUNT=10

LOCAL_LDFLAGS += -Wl,--gc-sections,--strip-all -llog
LOCAL_ARM_MODE := arm

LOCAL_C_INCLUDES += $(MAIN_LOCAL_PATH)
LOCAL_C_INCLUDES += $(LOCAL_PATH)
LOCAL_C_INCLUDES += $(LOCAL_PATH)/Includes
LOCAL_C_INCLUDES += $(LOCAL_PATH)/Il2Cpp/xdl/include

FILE_LIST := $(wildcard $(LOCAL_PATH)/Il2Cpp/xdl/*.c)

LOCAL_SRC_FILES := $(FILE_LIST:$(LOCAL_PATH)/%=%)
LOCAL_SRC_FILES += native-lib.cpp \
                   Il2Cpp/il2cpp_dump.cpp

LOCAL_LDLIBS := -llog -landroid -lGLESv2 -ldl
include $(BUILD_SHARED_LIBRARY)
