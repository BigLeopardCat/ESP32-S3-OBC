# u8g2 本地组件（vendored from https://github.com/olikraus/u8g2, v2.37.1）
# 注：IDF 默认 -fdata-sections + --gc-sections，未引用的字体会被链接器剔除，
#     固件里只会保留实际用到的字体（wqy12 GB2312）。
file(GLOB U8G2_SRCS "csrc/*.c")
idf_component_register(
    SRCS ${U8G2_SRCS}
    INCLUDE_DIRS "csrc"
    REQUIRES driver esp_timer esp_rom log
)
