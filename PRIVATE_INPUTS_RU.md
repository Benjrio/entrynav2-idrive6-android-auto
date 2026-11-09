# Локальные inputs для сборки

**Русский** | [English](PRIVATE_INPUTS_EN.md)

После чистого клонирования предоставить из своей офлайн-копии зависимостей следующие пути с неизменной структурой:

- `vendor/` — ARM hard-float sysroot/GCC support, headers/libs FFmpeg/OpenSSL,
  оригиналы библиотек HU для линковки.
- `runtime/aa/scene/` — winston и libwlxs-genivi.so нужного профиля ГУ.
- `runtime/aa/identity/` — phone.crt и phone.key: совместимая AA TLS identity.
  Предоставляется локально.
- `assets/OwnAndroidAuto_Images.zip` — ZIP с иконкой `1.png`.

Новые произвольные firmware libraries не гарантируют совместимость: ABI guards
в src/profile.hpp требуют проверенного профиля.
`build.py` сообщает об отсутствии обязательных inputs до запуска компилятора.
