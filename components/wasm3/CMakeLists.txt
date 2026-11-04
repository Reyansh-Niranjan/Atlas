idf_component_register(
    SRCS
        "m3_api_libc.c"
        "m3_bind.c"
        "m3_code.c"
        "m3_compile.c"
        "m3_core.c"
        "m3_emit.c"
        "m3_env.c"
        "m3_exec.c"
        "m3_function.c"
        "m3_info.c"
        "m3_module.c"
        "m3_parse.c"
    INCLUDE_DIRS
        "."
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -fno-strict-aliasing
    -Wno-error
    -Wno-strict-aliasing
    -Wno-unused-parameter
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-sign-compare
)
