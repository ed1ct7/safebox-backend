# sbx_embed_web(<target> <dist_dir>) - вшивание фронта в exe, пока заглушка

function(sbx_embed_web target dist_dir)
    message(FATAL_ERROR
        "sbx_embed_web: вшивание фронта еще не сделано (SAFEBOX_WEB_DIST='${dist_dir}')")
endfunction()
