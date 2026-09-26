./page_server.py


gcc -O0 -g -pthread demo.c -o demo &&  systemd-run --user --scope -p MemoryMax=1M -p MemorySwapMax=0  ./demo
