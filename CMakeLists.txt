cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# wolfSSL and wolfSSH (the SSH console), the same settings in every file that includes them, or
# their structs differ: user_settings.h, Ed25519 host keys (which wolfSSH wants streaming verify
# for), and wolfSSL's own ESP32 crypto-hardware port off, since it doesn't build against ESP-IDF 6
# (the software code is used instead; a login's key exchange takes a little longer). XMALLOC_USER:
# their allocations go to main/ssh_console.cpp's XMALLOC, XFREE and XREALLOC (PSRAM first). Without
# it, wolfSSL on FreeRTOS calls pvPortMalloc directly, which is internal RAM only, and ignores
# wolfSSL_SetAllocators.
idf_build_set_property(COMPILE_DEFINITIONS
  "WOLFSSL_USER_SETTINGS;XMALLOC_USER;WOLFSSL_ED25519_STREAMING_VERIFY;NO_ESP32_CRYPT=;NO_WOLFSSL_ESP32_CRYPT_HASH=;NO_WOLFSSL_ESP32_CRYPT_AES=;NO_WOLFSSL_ESP32_CRYPT_RSA_PRI="
  APPEND)
project(emcool)

# Their own sources: C17 (ESP-IDF 6 builds C23, where `thread_local` is a keyword one of wolfSSL's
# enums uses as a name), and their #warnings left as warnings.
foreach(component wolfssl__wolfssl wolfssl__wolfssh)
  idf_component_get_property(library ${component} COMPONENT_LIB)
  target_compile_options(${library} PRIVATE -Wno-error=cpp $<$<COMPILE_LANGUAGE:C>:-std=gnu17>)
endforeach()
