#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#define PAGE_SIZE 4096
#define NUM_PAGES 256

#define SERVER_IP   "127.0.0.1"
#define SERVER_PORT 8080


static int uffd;
static char *region;



static int
http_get_page(int page, char *buffer)
{
    int sock;
    struct sockaddr_in addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);

    if (inet_pton(AF_INET, SERVER_IP, &addr.sin_addr) != 1) {
        perror("inet_pton");
        close(sock);
        return -1;
    }

    if (connect(sock,
                (struct sockaddr *)&addr,
                sizeof(addr)) == -1) {
        perror("connect");
        close(sock);
        return -1;
    }


    char request[256];

    snprintf(request,
             sizeof(request),
             "GET /page/%d HTTP/1.0\r\n"
             "Host: " SERVER_IP "\r\n"
             "Connection: close\r\n"
             "\r\n",
             page);

    if (write(sock, request, strlen(request)) < 0) {
        perror("write");
        close(sock);
        return -1;
    }

    char response[8192];
    size_t total = 0;

    while (total < sizeof(response) - 1) {
        ssize_t n = read(sock,
                         response + total,
                         sizeof(response) - 1 - total);

        if (n == 0)
            break;

        if (n < 0) {
            perror("read");
            close(sock);
            return -1;
        }

        total += n;
    }

    close(sock);

    response[total] = '\0';


    char *body = strstr(response, "\r\n\r\n");

    if (!body) {
        fprintf(stderr, "Invalid HTTP response\n");
        return -1;
    }

    body += 4;


    size_t body_size = total - (body - response);

    if (body_size > PAGE_SIZE)
        body_size = PAGE_SIZE;

    memcpy(buffer, body, body_size);

    /*
     * Jeżeli serwer zwrócił mniej niż 4096 bajtów,
     * wyzeruj resztę.
     */
    if (body_size < PAGE_SIZE)
        memset(buffer + body_size,
               0,
               PAGE_SIZE - body_size);

    return 0;
}


/*
 * Thread obsługujący page faults.
 */
static void *
fault_handler(void *arg)
{
    (void)arg;

    struct pollfd pollfd;

    char page[PAGE_SIZE];


    for (;;) {

        pollfd.fd = uffd;
        pollfd.events = POLLIN;

        int n = poll(&pollfd, 1, -1);
        // int n = poll(&pollfd, 1, 100);

        if (n == -1) {
            perror("poll");
            exit(EXIT_FAILURE);
        }


        struct uffd_msg msg;

        ssize_t nr = read(uffd, &msg, sizeof(msg));

        if (nr <= 0) {
            perror("read(userfaultfd)");
            exit(EXIT_FAILURE);
        }


        if (msg.event != UFFD_EVENT_PAGEFAULT) {
            fprintf(stderr,
                    "Unexpected userfault event: %u\n",
                    msg.event);

            continue;
        }


        /*
         * Adres pamięci, którego proces próbował użyć.
         */
        unsigned long fault_address =
            msg.arg.pagefault.address;


        /*
         * Zaokrąglamy adres do początku strony.
         */
        unsigned long page_address =
            fault_address & ~(PAGE_SIZE - 1);


        /*
         * Obliczamy numer strony.
         */
        int page_number =
            (page_address - (unsigned long)region)
            / PAGE_SIZE;


        printf("PAGE FAULT: page=%d\n",
               page_number);


        /*
         * Pobieramy stronę.
         */
        if (http_get_page(page_number, page) != 0) {
            fprintf(stderr,
                    "HTTP fetch failed for page %d\n",
                    page_number);

            exit(EXIT_FAILURE);
        }


        /*
         * Dostarczamy pobraną stronę do procesu.
         */
        struct uffdio_copy copy;

        memset(&copy, 0, sizeof(copy));

        copy.src =
            (unsigned long)page;

        copy.dst =
            page_address;

        copy.len =
            PAGE_SIZE;

        copy.mode = 0;


        if (ioctl(uffd,
                  UFFDIO_COPY,
                  &copy) == -1) {

            perror("UFFDIO_COPY");
            exit(EXIT_FAILURE);
        }


        printf("  -> fetched page %d from LAN\n",
               page_number);
    }

    return NULL;
}


int
main(void)
{


    size_t length = NUM_PAGES * PAGE_SIZE;

    /*
     * 1. Tworzymy userfaultfd.
        Opcja 2 — włączenie userfaultfd dla zwykłych użytkowników
            Możesz też zmienić sysctl:
            cat /proc/sys/vm/unprivileged_userfaultfd
            Jeśli dostajesz:
            0
            tymczasowo:
            sudo sysctl -w vm.unprivileged_userfaultfd=1
            albo UFFD_USER_MODE_ONLY
     */
    uffd = syscall(
        SYS_userfaultfd,
        O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY
    );

    if (uffd == -1) {
        perror("userfaultfd");

        fprintf(stderr,
                "\nJeżeli dostajesz EPERM, sprawdź "
                "ustawienia userfaultfd w kernelu.\n");

        return 1;
    }


    /*
     * 2. Włączamy API.
     */
    struct uffdio_api api;

    memset(&api, 0, sizeof(api));

    api.api = UFFD_API;

    if (ioctl(uffd,
              UFFDIO_API,
              &api) == -1) {

        perror("UFFDIO_API");
        return 1;
    }


    /*
     * 3. Rezerwujemy anonimową pamięć.
     */
    region = mmap(
        NULL,
        length,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );

    if (region == MAP_FAILED) {
        perror("mmap");
        return 1;
    }


    /*
     * 4. Rejestrujemy region jako MISSING.
     */
    struct uffdio_register reg;

    memset(&reg, 0, sizeof(reg));

    reg.range.start =
        (unsigned long)region;

    reg.range.len =
        length;

    reg.mode =
        UFFDIO_REGISTER_MODE_MISSING;


    if (ioctl(uffd,
              UFFDIO_REGISTER,
              &reg) == -1) {

        perror("UFFDIO_REGISTER");
        return 1;
    }


    /*
     * 5. Uruchamiamy thread obsługujący page faults.
     */
    pthread_t thread;

    if (pthread_create(
            &thread,
            NULL,
            fault_handler,
            NULL) != 0) {

        perror("pthread_create");
        return 1;
    }
    pthread_detach(thread);


    printf("Virtual memory: %zu bytes\n", length);
    printf("Starting test...\n\n");


    /*
     * 6. Teraz najważniejsza część.
     *
     * Ten kod wygląda jak zwykły dostęp do RAM.
     *
     * Ale pierwsze odwołanie do każdej strony powoduje:
     *
     *   page fault
     *       ↓
     *   userfaultfd
     *       ↓
     *   HTTP GET
     *       ↓
     *   UFFDIO_COPY
     *       ↓
     *   instrukcja zostaje wznowiona
     */
    for (int i = 0; i < NUM_PAGES; i++) {

        printf("Reading page %d...\n", i);
        // musimy dotknąć strony inaczej nie będzie page fault
        volatile unsigned char value = region[i * PAGE_SIZE];
        char * c = region + i * PAGE_SIZE;
        printf("value = %s\n\n", c);

        /** 
        * przy ograniczeniu pamięci scope'em (i wyłaczonym swapie)
        * bez tego oczekujemy crasha
        *    Reading page 183...
        *    PAGE FAULT: page=183
        *    zsh: killed     systemd-run --user --scope -p MemoryMax=1M -p MemorySwapMax=0 ./demo
        *
        */
        madvise(c, PAGE_SIZE, MADV_DONTNEED);
    }

    /*
     * Drugi odczyt tych samych stron
     * NIE powinien powodować HTTP requestów, chyba że strony już nie ma
     * 
     */
    volatile unsigned char value = region[0 * PAGE_SIZE];
    printf("Reading page 0 again...\n");
    printf("value = %s\n", region);

    return 0;
}

