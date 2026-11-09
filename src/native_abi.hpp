// Declarations reconstructed from the original Linux 002_156_007 ELF files.
// Not a BMW SDK. Layouts are restricted by the build-ID gate in native_app.cpp.
#pragma once
#include <stdint.h>
#include <stddef.h>

inline void* operator new(size_t, void* p) noexcept { return p; }
inline void operator delete(void*, void*) noexcept {}

namespace bmw {
class IDispatcher;
class Bundle;
enum RHMIInterfaceVersion { RHMI_INTERFACE_VERSION_1 = 1 };
template<class T> class BasicString {
public:
    explicit BasicString(const T*);
    BasicString(const BasicString&);
    ~BasicString();
    const T* data() const { return value; }
private:
    T* value;
};
// BufferStream C1/D1: six words, including its original virtual-table pointer.
class BufferStream {
public:
    ~BufferStream();
private:
    uint32_t words[6];
};
// Image has no exported destructor: the original inlined destructor destroys
// BufferStream at +4 then BasicString at +0. C1(bytes,size) copies the payload.
class Image {
public:
    Image(const unsigned char*, unsigned);
private:
    BasicString<char> path;
    BufferStream bytes;
    uint32_t loadedAndPadding;
};
class Parcel {
public:
    void readBool(bool&) const;
};
// dispatch() invokes Runnable::run through vptr+8. Queue messages do not own
// this pointer: PluginApp stop clears its queue before destroying the object.
class Runnable {
public:
    virtual ~Runnable() {}
    virtual void run() = 0;
};
template<class R, class A, class B, class C, class D> struct Delegate {
    void* object;
    R (*call)(void*, A);
};
typedef Delegate<bool, const Parcel&, void, void, void> ActionDelegate;

class alignas(8) PluginApp {
public:
    PluginApp(IDispatcher&, BasicString<char>, unsigned, unsigned,
              RHMIInterfaceVersion, const Bundle&, const Bundle&);
    ~PluginApp();
    void addHandler(unsigned, const ActionDelegate&);
    void addHandler(unsigned, unsigned, const ActionDelegate&);
    bool updateModel(unsigned, const Image&);
    bool updateModel(unsigned, const char*);
    bool updateModel(unsigned, int);
    void post(Runnable*, unsigned);
    bool triggerEvent(unsigned);
    bool activate();
    void deactivate();
    bool onInit();
    void onFinalize();
private:
    // Native constructor stores through +0x15c; reserve more without placing
    // our fields in that storage. Never use sizeof as the native object size.
    uint32_t opaque[128];
};
}
static_assert(sizeof(void*) == 4, "ARM32 only");
static_assert(sizeof(bmw::BasicString<char>) == 4, "BasicString ABI");
static_assert(sizeof(bmw::BufferStream) == 24, "BufferStream ABI");
static_assert(sizeof(bmw::Image) == 32, "Image ABI");
static_assert(sizeof(bmw::ActionDelegate) == 8, "Delegate ABI");

struct PluginAppInfo {
    uint16_t major, minor;
    const char *name, *vendor;
    uint32_t rhmiVersion;
    bmw::PluginApp* (*create)(bmw::IDispatcher&, const char*, const bmw::Bundle&, const bmw::Bundle&);
    void (*destroy)(void*);
};
static_assert(sizeof(PluginAppInfo) == 24, "ManagedPluginApp factory ABI");
