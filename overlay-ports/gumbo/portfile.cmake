# overlay-ports/gumbo/portfile.cmake
#
# 为什么存在这个覆盖端口（详见 docs/03-决策记录.md ADR-010）：
#   vcpkg 内置 gumbo 端口（0.13.2）钉的归档哈希来自 codeberg 的 tag 归档
#   https://codeberg.org/gumbo-parser/gumbo-parser/archive/0.13.2.tar.gz
#   Forgejo 的 tag 归档不是字节稳定的（tag 被重指 / 服务端重新打包都会改变
#   归档字节），导致构建时报 "download ... had an unexpected hash"。
#
#   本覆盖端口做两件事：
#     1. 改为按**提交号**拉取自包含归档，锚定不可变对象；
#     2. 用实测的 SHA512 替换过期哈希（改动随端口版本号 port-version 递增）。
#
#   内容校验方式：解压后顶层为 gumbo-parser/，与上游 tag 0.13.2
#   （提交 322c54c178590ba42b8b04e8c0e4840595a1f717）一致。
#
# 维护提示：若上游再次改动归档导致哈希不符，重新计算实际 SHA512 并递增
#   vcpkg.json 的 port-version；若 vcpkg 内置端口已自行修正，可删除本覆盖端口。

vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_download_distfile(ARCHIVE
    URLS "https://codeberg.org/gumbo-parser/gumbo-parser/archive/322c54c178590ba42b8b04e8c0e4840595a1f717.tar.gz"
    FILENAME "gumbo-${VERSION}-322c54c1.tar.gz"
    SHA512  9f59965e68ba2e4f5884d52c4126b62cdbefee158816334e317a0d07f10ba927be653490c69fc5b0f52eed1decf0a51715bb726aa84546a2d04cde5805e4a399
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
    SOURCE_BASE "${VERSION}"
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()

vcpkg_copy_pdbs()

vcpkg_cmake_config_fixup(PACKAGE_NAME unofficial-gumbo CONFIG_PATH share/unofficial-gumbo)

vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/doc/COPYING")
