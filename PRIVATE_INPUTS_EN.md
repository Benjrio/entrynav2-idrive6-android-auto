# Local build inputs

[Русский](PRIVATE_INPUTS_RU.md) | **English**

After a clean clone, provide the following paths from your offline copy of the
dependencies, preserving their directory structure:

- `vendor/`: ARM hard-float sysroot/GCC support, FFmpeg/OpenSSL headers and
  libraries, and original head unit libraries for linking.
- `runtime/aa/scene/`: winston and libwlxs-genivi.so for the required head unit profile.
- `runtime/aa/identity/`: phone.crt and phone.key, a compatible AA TLS identity.
  Provided locally.
- `assets/OwnAndroidAuto_Images.zip`: a ZIP archive containing the icon `1.png`.

Arbitrary replacement firmware libraries do not guarantee compatibility:
the ABI guards in `src/profile.hpp` require a validated profile.
`build.py` reports missing mandatory inputs before invoking the compiler.
