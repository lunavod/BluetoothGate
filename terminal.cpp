#include <stdio.h>
#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/rfcomm.h>

// TODO: переписать под протокол

int main(int argc, char **argv)
{
    struct sockaddr_rc addr = { 0 };
    int s, status;
    char dest[18] = "98:D3:11:F8:3E:B1";

    s = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);

    addr.rc_family = AF_BLUETOOTH;
    addr.rc_channel = (uint8_t) 1;
    str2ba( dest, &addr.rc_bdaddr );

    status = connect(s, (struct sockaddr *)&addr, sizeof(addr));

    while (true) {
        char msg[256];
        char buf[1024] = { 0 };
        int bytes_read;

        std::cout << "> ";
        std::cin >> msg;
        status = write(s, msg, strlen(msg));
        
        bytes_read = read(s, buf, sizeof(buf));
        if( bytes_read > 0 ) {
            printf("%s\n", buf);
        }
    }

    if( status < 0 ) perror("uh oh");

    close(s);
    return 0;
}