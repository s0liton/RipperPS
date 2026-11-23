cmake_minimum_required(VERSION 3.16)

project(ripper C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# gnu11: POSIX/BSD functions (mkdtemp, clock_gettime, strnlen) stay visible.
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(RIPPER_WERROR "Treat warnings as errors" ON)

add_library(ripper_core STATIC
  src/core/aacs.c
  src/core/aes.c
  src/core/audio_out.c
  src/core/cd_ecc.c
  src/core/cdrom.c
  src/core/chd.c
  src/core/ciso.c
  src/core/config.c
  src/core/css.c
  src/core/cue.c
  src/core/diag.c
  src/core/disc.c
  src/core/drive.c
  src/core/dvd.c
  src/core/exfat.c
  src/core/fpkg.c
  src/core/game.c
  src/core/hash.c
  src/core/image_drive.c
  src/core/iso9660.c
  src/core/keydb.c
  src/core/log.c
  src/core/mmc.c
  src/core/omnidrive.c
  src/core/pfs.c
  src/core/pkg.c
  src/core/ps3.c
  src/core/reader.c
  src/core/rip.c
  src/core/rip_cd.c
  src/core/rsa.c
  src/core/remux.c
  src/core/rvz.c
  src/core/sce_elf.c
  src/core/sfo.c
  src/core/transport.c
  src/core/udf.c
  src/core/ufs2.c
  src/core/util.c
  src/core/wbfs.c
  src/core/xbox.c
  src/core/xiso.c
  src/core/zip.c
)
target_include_directories(ripper_core PUBLIC include)
# libdvdcss (GPL-2.0-or-later) for DVD-Video's CSS; its drive commands come
# from src/core/css.c instead of its ioctl.c.
add_library(dvdcss STATIC
  third_party/libdvdcss/src/css.c
  third_party/libdvdcss/src/device.c
  third_party/libdvdcss/src/error.c
  third_party/libdvdcss/src/libdvdcss.c
)
target_include_directories(dvdcss PUBLIC third_party/libdvdcss/src)
target_compile_options(dvdcss PRIVATE -w)
target_link_libraries(ripper_core PRIVATE dvdcss)
find_package(Threads REQUIRED)
if(PS5)
  # The SDK's pacbrew libraries (zlib, zstd) live under its homebrew prefix.
  target_link_libraries(ripper_core PUBLIC z zstd Threads::Threads)
else()
  find_package(ZLIB REQUIRED)
  find_library(ZSTD_LIBRARY zstd REQUIRED)
  target_link_libraries(ripper_core PUBLIC ZLIB::ZLIB ${ZSTD_LIBRARY} Threads::Threads)
endif()
# snprintf cutting a string to its buffer is meant, here and in the service.
target_compile_options(ripper_core PRIVATE -Wall -Wextra -Wno-format-truncation)
if(RIPPER_WERROR)
  target_compile_options(ripper_core PRIVATE -Werror)
endif()

# The web UI, with its stylesheets and font inlined, as a C string.
set(GEN ${CMAKE_BINARY_DIR}/gen)
file(GLOB UI_THEMES ${CMAKE_SOURCE_DIR}/ui/themes/*.css)
add_custom_command(
  OUTPUT ${CMAKE_SOURCE_DIR}/build/gen/inter-font.css
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/build/gen
  COMMAND python3 tools/font_css.py third_party/inter-latin.woff2 build/gen/inter-font.css Inter
  DEPENDS tools/font_css.py third_party/inter-latin.woff2
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_custom_command(
  OUTPUT ${GEN}/ui_index.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
  COMMAND python3 tools/embed.py ui/index.html ${GEN}/ui_index.h UI_INDEX_HTML
  DEPENDS ui/index.html ui/style.css ui/ripper.css ${UI_THEMES} tools/embed.py ${CMAKE_SOURCE_DIR}/build/gen/inter-font.css
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_custom_command(
  OUTPUT ${GEN}/gamedb_data.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${GEN}
  COMMAND python3 tools/embed.py third_party/gamedb/gamedb.tsv ${GEN}/gamedb_data.h GAMEDB_GZ --gzip
  DEPENDS third_party/gamedb/gamedb.tsv tools/embed.py
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# The service: drive watcher, jobs, destinations, the API and web UI.
add_library(ripper_service STATIC
  src/service/bench.c
  src/service/catalog.c
  src/service/deliver.c
  src/service/details.c
  src/service/deliver_ftp.c
  src/service/deliver_romm.c
  src/service/deliver_nfs.c
  src/service/deliver_smb.c
  src/service/drivetest.c
  src/service/langs.c
  src/service/disc_report.c
  src/service/drives.c
  src/service/gamedb.c
  src/service/http.c
  src/service/jobs.c
  src/service/musicbrainz.c
  src/service/remote.c
  src/service/service.c
  src/service/settings.c
  src/service/stats.c
  src/service/svc.c
  src/service/web.c
  third_party/cJSON.c
  ${GEN}/ui_index.h
  ${GEN}/gamedb_data.h
)
target_include_directories(ripper_service PUBLIC src/service third_party PRIVATE ${GEN})
target_link_libraries(ripper_service PUBLIC ripper_core)
target_compile_options(ripper_service PRIVATE -Wall -Wextra -Wno-format-truncation)
# Network destinations, each when its library is there.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(CURL QUIET IMPORTED_TARGET libcurl)
  pkg_check_modules(SMB2 QUIET IMPORTED_TARGET libsmb2)
  pkg_check_modules(NFS QUIET IMPORTED_TARGET libnfs)
endif()
if(CURL_FOUND)
  target_compile_definitions(ripper_service PUBLIC RIPPER_HAVE_CURL)
  target_link_libraries(ripper_service PUBLIC PkgConfig::CURL)
endif()
if(SMB2_FOUND)
  target_compile_definitions(ripper_service PUBLIC RIPPER_HAVE_SMB2)
  target_link_libraries(ripper_service PUBLIC PkgConfig::SMB2)
endif()
if(NFS_FOUND)
  target_compile_definitions(ripper_service PUBLIC RIPPER_HAVE_NFS)
  target_link_libraries(ripper_service PUBLIC PkgConfig::NFS)
endif()
set_source_files_properties(third_party/cJSON.c PROPERTIES COMPILE_OPTIONS "-w")
if(RIPPER_WERROR)
  target_compile_options(ripper_service PRIVATE -Werror)
endif()

if(PS5)
  # Configured through the PS5 payload SDK: prospero-cmake -B build/ps5
  message(STATUS "ripper: PS5 payload build")
  add_executable(ripper
    src/platform/ps5/cam_transport.c
    src/platform/ps5/main.c
    src/platform/ps5/platform.c
  )
  target_include_directories(ripper PRIVATE src/platform/ps5)
  target_link_libraries(ripper PRIVATE ripper_service SceNotification SceSystemService)
  target_compile_options(ripper PRIVATE -Wall -Wextra)
  set_target_properties(ripper PROPERTIES OUTPUT_NAME ripper.elf)

  add_custom_target(deploy
    USES_TERMINAL
    COMMAND ${PS5_DEPLOY} -h $ENV{PS5_HOST} -p 9021 ripper.elf
    DEPENDS ripper
  )
else()
  message(STATUS "ripper: host build (CLI and tests)")
  add_executable(ripper-cli
    src/platform/linux/main.c
    src/platform/linux/sgio_transport.c
  )
  target_link_libraries(ripper-cli PRIVATE ripper_core)
  target_compile_options(ripper-cli PRIVATE -Wall -Wextra)

  # The service on a PC, with a virtual drive: the web UI at http://localhost:8790.
  add_executable(ripperd
    src/platform/linux/platform.c
    src/platform/linux/ripperd.c
    src/platform/linux/sgio_transport.c
  )
  target_include_directories(ripperd PRIVATE src/platform/linux)
  target_link_libraries(ripperd PRIVATE ripper_service)
  target_compile_options(ripperd PRIVATE -Wall -Wextra)

  enable_testing()
  add_executable(ripper-tests
    tests/fake_drive.c
    tests/test_firmware.c
    tests/test_formats.c
    tests/test_identify.c
    tests/test_langs.c
    tests/test_iso_extract.c
    tests/test_game_images.c
    tests/test_wbfs.c
    tests/test_rvz.c
    tests/test_aacs.c
    tests/test_fpkg.c
    src/service/langs.c
    third_party/cJSON.c
    tests/udf_builder.c
    tests/iso_builder.c
    tests/test_main.c
    tests/test_rip.c
  )
  target_include_directories(ripper-tests PRIVATE tests src/service third_party)
  target_link_libraries(ripper-tests PRIVATE ripper_core)
  target_compile_options(ripper-tests PRIVATE -Wall -Wextra)
  add_test(NAME ripper-tests COMMAND ripper-tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()
