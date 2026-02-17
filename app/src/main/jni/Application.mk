APP_ABI := all
APP_STL := c++_static
APP_PLATFORM := android-21
APP_OPTIM := release
APP_THIN_ARCHIVE := true
APP_PIE := true
# Frida требует RTTI и исключения внутри себя, 
# но для твоего кода мы их оставили выключенными в Android.mk
