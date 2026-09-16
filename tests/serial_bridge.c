#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <signal.h>

static int set_serial_attribs(int fd, int speed) {
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        return -1;
    }

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8; // 8-bit chars
    tty.c_iflag &= ~IGNBRK;                     // disable break processing
    tty.c_lflag = 0;                            // no signaling chars, no echo, no canonical processing
    tty.c_oflag = 0;                            // no remapping, no delays
    tty.c_cc[VMIN]  = 0;                        // read doesn't block
    tty.c_cc[VTIME] = 1;                        // 0.1 seconds read timeout

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);     // shut off xon/xoff ctrl
    tty.c_cflag |= (CLOCAL | CREAD);            // ignore modem controls, enable reading
    tty.c_cflag &= ~(PARENB | PARODD);          // shut off parity
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    const char *dev = "/dev/ttyUSB2";
    int port = 2000;
    if (argc > 1) dev = argv[1];
    if (argc > 2) port = atoi(argv[2]);

    int sfd = open(dev, O_RDWR | O_NOCTTY | O_SYNC);
    if (sfd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", dev, strerror(errno));
        return 1;
    }

    if (set_serial_attribs(sfd, B115200) < 0) {
        fprintf(stderr, "Failed to set serial attribs\n");
        return 1;
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port);

    if (bind(lfd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("bind");
        return 1;
    }
    listen(lfd, 1);
    printf("Listening on port %d for %s at 115200 baud\n", port, dev);
    fflush(stdout);

    int logfd = open("/tmp/serial_bridge.log", O_WRONLY | O_CREAT | O_APPEND, 0666);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t clen = sizeof(client_addr);
        int cfd = accept(lfd, (struct sockaddr *)&client_addr, &clen);
        if (cfd < 0) continue;

        printf("Client connected from %s\n", inet_ntoa(client_addr.sin_addr));
        fflush(stdout);

        while (1) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sfd, &rfds);
            FD_SET(cfd, &rfds);
            int maxfd = (sfd > cfd ? sfd : cfd) + 1;

            int ret = select(maxfd, &rfds, NULL, NULL, NULL);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }

            char buf[1024];
            if (FD_ISSET(sfd, &rfds)) {
                int n = read(sfd, buf, sizeof(buf));
                if (n > 0) {
                    if (write(cfd, buf, n) < 0) break;
                    if (logfd >= 0) write(logfd, buf, n);
                }
            }
            if (FD_ISSET(cfd, &rfds)) {
                int n = read(cfd, buf, sizeof(buf));
                if (n <= 0) break; // client disconnected
                if (write(sfd, buf, n) < 0) break;
            }
        }
        close(cfd);
        printf("Client disconnected\n");
        fflush(stdout);
    }

    close(sfd);
    close(lfd);
    if (logfd >= 0) close(logfd);
    return 0;
}
