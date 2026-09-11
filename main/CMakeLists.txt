file(GLOB_RECURSE MY_SRCS
    "./hal/*.c"
    "./hal/*.cc"
    "./hal/*.cpp"
    "./apps/*.c"
    "./apps/*.cc"
    "./apps/*.cpp"
)

set(MY_INCLUDE_DIRS
    "."
    "./assets"
    "./hal"
    "./apps"
    # dhex API headers, so the application source keeps its own include
    # paths unchanged. See apps/app_dhex/dhex_api/README.md.
    "./apps/app_dhex/dhex_api"
)

file(GLOB EXTERN_FILES assets/*)
# assets/ currently holds only a placeholder header. EMBED_FILES expects
# binary blobs, and embedding a header makes the PlatformIO build fail
# while generating its .S stub.
list(FILTER EXTERN_FILES EXCLUDE REGEX "\\.h$")

idf_component_register(SRCS "main.cpp" ${MY_SRCS}
                    INCLUDE_DIRS ${MY_INCLUDE_DIRS}
                    EMBED_FILES ${EXTERN_FILES}
)
