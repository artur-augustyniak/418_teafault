# 418 - I'm a Teafault

Repository accompanying the ["418 - I'm a Teafault"](article/418_teafault.pdf) article.

The repository demonstrates using Linux `userfaultfd` to handle page faults in userspace and resolve missing pages by fetching their contents over HTTP.

## Repository contents

* `client.c` - `userfaultfd` client and userspace page-fault handler
* `server.py` - minimal HTTP server serving 4 KiB pages
* `article/` - LaTeX source of the article

## Running

Start the HTTP server:

```bash
python3 server.py
```

Compile and run the client:

```bash
gcc -pthread client.c -o client
./client
```

To run the client with a small memory limit:

```bash
systemd-run --user --scope -p MemoryMax=1M -p MemorySwapMax=0 ./client
```

## Generate article

```bash
cd ./article
pdflatex 418_teafault.tex
```