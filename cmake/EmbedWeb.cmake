# sbx_embed_web(<target> <dist_dir>) - вшивает собранный фронт в exe.
# файл работает в двух режимах: как include (функция внизу) и как cmake -P скрипт,
# который генерит web_assets_data.cpp из файлов dist

# режим скрипта: генерация web_assets_data.cpp
if(CMAKE_SCRIPT_MODE_FILE AND DEFINED SBX_DIST_DIR)
    file(GLOB_RECURSE _files LIST_DIRECTORIES false RELATIVE "${SBX_DIST_DIR}" "${SBX_DIST_DIR}/*")
    list(SORT _files)
    string(REPEAT "[0-9a-f]" 64 _line_pattern) # 32 байта на строку исходника

    set(_arrays "")
    set(_table "")
    set(_index 0)
    foreach(_rel IN LISTS _files)
        get_filename_component(_ext "${_rel}" LAST_EXT)
        string(TOLOWER "${_ext}" _ext)
        if(_ext STREQUAL ".html" OR _ext STREQUAL ".htm")
            set(_mime "text/html; charset=utf-8")
        elseif(_ext STREQUAL ".js" OR _ext STREQUAL ".mjs")
            set(_mime "text/javascript; charset=utf-8")
        elseif(_ext STREQUAL ".css")
            set(_mime "text/css; charset=utf-8")
        elseif(_ext STREQUAL ".json" OR _ext STREQUAL ".map" OR _ext STREQUAL ".webmanifest")
            set(_mime "application/json")
        elseif(_ext STREQUAL ".svg")
            set(_mime "image/svg+xml")
        elseif(_ext STREQUAL ".png")
            set(_mime "image/png")
        elseif(_ext STREQUAL ".jpg" OR _ext STREQUAL ".jpeg")
            set(_mime "image/jpeg")
        elseif(_ext STREQUAL ".webp")
            set(_mime "image/webp")
        elseif(_ext STREQUAL ".ico")
            set(_mime "image/x-icon")
        elseif(_ext STREQUAL ".woff")
            set(_mime "font/woff")
        elseif(_ext STREQUAL ".woff2")
            set(_mime "font/woff2")
        elseif(_ext STREQUAL ".txt")
            set(_mime "text/plain; charset=utf-8")
        elseif(_ext STREQUAL ".wasm")
            set(_mime "application/wasm")
        else()
            set(_mime "application/octet-stream")
        endif()

        file(READ "${SBX_DIST_DIR}/${_rel}" _hex HEX)
        string(LENGTH "${_hex}" _hex_length)
        math(EXPR _size "${_hex_length} / 2")
        string(REGEX REPLACE "(${_line_pattern})" "\\1\n" _hex "${_hex}")
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")

        # завершающий 0 - массив не бывает пустым; в size он не входит
        string(APPEND _arrays "const unsigned char kData${_index}[] = {\n${_bytes}0};\n")
        string(APPEND _table "    {\"/${_rel}\", \"${_mime}\", kData${_index}, ${_size}},\n")
        math(EXPR _index "${_index} + 1")
    endforeach()

    if(_index EQUAL 0)
        message(FATAL_ERROR "sbx_embed_web: в ${SBX_DIST_DIR} нет файлов")
    endif()

    file(WRITE "${SBX_OUTPUT}"
        "// Сгенерировано cmake/EmbedWeb.cmake из frontend/dist - не редактировать.\n"
        "#include \"web_assets.hpp\"\n\n"
        "namespace safebox::daemon {\nnamespace {\n\n"
        "${_arrays}\n"
        "const EmbeddedFile kFiles[] = {\n${_table}};\n\n"
        "} // namespace\n\n"
        "std::span<const EmbeddedFile> embeddedFiles() noexcept { return kFiles; }\n\n"
        "} // namespace safebox::daemon\n")
    return()
endif()

# режим include: функция для CMakeLists
set(SBX_EMBED_WEB_SCRIPT "${CMAKE_CURRENT_LIST_FILE}")

function(sbx_embed_web target dist_dir)
    get_filename_component(_dist "${dist_dir}" ABSOLUTE BASE_DIR "${PROJECT_SOURCE_DIR}")
    if(NOT EXISTS "${_dist}/index.html")
        message(FATAL_ERROR
            "sbx_embed_web: в '${_dist}' нет index.html - сначала соберите фронтенд "
            "(npm run build) или уберите SAFEBOX_WEB_DIST")
    endif()
    file(GLOB_RECURSE _files CONFIGURE_DEPENDS LIST_DIRECTORIES false "${_dist}/*")
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/web_assets_data.cpp")
    add_custom_command(
        OUTPUT "${_out}"
        COMMAND "${CMAKE_COMMAND}" "-DSBX_DIST_DIR=${_dist}" "-DSBX_OUTPUT=${_out}"
                -P "${SBX_EMBED_WEB_SCRIPT}"
        DEPENDS ${_files} "${SBX_EMBED_WEB_SCRIPT}"
        COMMENT "Встраивание фронтенда из ${_dist}"
        VERBATIM)
    target_sources(${target} PRIVATE "${_out}")
    # сгенерированный .cpp лежит в каталоге сборки, а web_assets.hpp - в исходниках
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(${target} PRIVATE SAFEBOX_HAS_WEB_ASSETS=1)
endfunction()
