# Centralised compiler warning configuration.

function(affineui_set_warnings target as_errors)
    set(_gcc_clang
        -Wall -Wextra -Wpedantic
        -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
        -Wunused -Woverloaded-virtual -Wconversion -Wsign-conversion
        -Wnull-dereference -Wdouble-promotion -Wformat=2 -Werror=format
        -Wimplicit-fallthrough
    )
    set(_msvc
        /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor
        /w14242 /w14254 /w14263 /w14265 /w14287 /we4289
        /w14296 /w14311 /w14545 /w14546 /w14547 /w14549 /w14555
        /w14619 /w14640 /w14826 /w14905 /w14906 /w14928
        /we4473 /we4477
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE ${_msvc})
        if(as_errors)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE ${_gcc_clang})
        if(as_errors)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
