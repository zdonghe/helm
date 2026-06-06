$srcs = ".\src\helm.c", ".\src\helm_cache.c", ".\src\helm_app.c", ".\src\helm_vd.c", ".\src\helm_sz.c", ".\src\helm_swap.c", ".\src\helm_max_min.c", ".\src\helm_uri.c", ".\src\helm_paste.c", ".\src\helm_log.c"
$cflags = "-std=c99 -O2 -flto -march=native -DNDEBUG=1 -DSTRIP_LOGS=1 -DUNICODE -D_UNICODE -s " +
"-fno-strict-aliasing -fmerge-all-constants -fno-unwind-tables " + "-ffunction-sections -fdata-sections -Wl,--gc-sections"

& gcc $cflags.Split() -municode -o helm.exe        @srcs -lole32 -loleaut32 -luser32 -lshell32 -ldwmapi -ladvapi32 -lntdll
# & gcc $cflags.Split() -mwindows -o kanata-bridge.exe ".\kanata-bridge\kanata-bridge.c" -lws2_32
