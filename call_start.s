    ldpa    .ulmld.pool,    %4
    ldfp    0(%4),	    %4
    call    %4,		    %0
    .align  8
.ulmld.pool
    .quad   _start
