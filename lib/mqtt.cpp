#include <cstring>
#include <unistd.h>

#define USERNAME_FLAG 0b10000000
#define PASSWORD_FLAG 0b01000000
#define WILL_RETAIN   0b00100000
#define WILL_QOS      0b00001000
#define WILL_FLAG     0b00000100
#define CLEAN_START   0b00000010

#define CONN_ACK 32
#define PUB_ACK  64
#define SUB_ACK  -112
#define PUB      48

struct conn_message {
    char flags;
    short int keep_alive;
    char *client_id;
    char *will_topic;
    char *will_message;
    char *username;
    char *password;
};

struct pub_message {
    bool dup = false;
    int qos = 0;
    bool retain = false;
    char *topic_name;
    short packet_id;
    char *payload;
};

struct sub_msg {
    char *topic_name;
    short packet_id;
    char qos = 0;
};

void substr(char *str, int from, int to, char *newstr) {
    int i = 0;
    for (; i < to - from; i++) {
        newstr[i] = str[from+i];
    }
    newstr[i+1] = '\0';
}

int decode(char* bytes) {
    int multiplier = 1;
    int value = 0;

    for (int i = 0; i < strlen(bytes); i++) {
        if (!bytes[i]) continue;
        char encodedByte = bytes[i];
        value += (encodedByte & 127) * multiplier;
        multiplier *= 128;
        if (multiplier > 128*128*128) return -1;
    }
    return value;
}

int encode(int x, char* buf) {
    int c = 0;
    do {
        char encodedByte = x % 128;
        x = x / 128;
        if (x) encodedByte = encodedByte | 128;
        buf[c] = encodedByte;
        c++;
    } while (x);
    return c;
}

int addString(int padding, char* buffer, char* str) {
    padding++;
    buffer[padding] = (strlen(str) >> (8*1)) & 0xff;
    padding++;
    buffer[padding] = (strlen(str) >> (8*0)) & 0xff;

    for (int i =0; i < strlen(str); i++) {
        padding++;
        buffer[padding] = str[i];
    }

    return padding;
}

char protocol_info[7] = {
    0b00000000,
    0b00000100,
    'M', 'Q', 'T', 'T',
    0b00000100
};

void addProtocolInfo(char* buff) {
    for (int i =0; i < 7; i++) {
        buff[i] = protocol_info[i];
    }
}

void mqtt_connect(int fd, conn_message msg) {
    char data[2048];
    addProtocolInfo(data);
    
    data[7] = msg.flags;
    data[8] = (msg.keep_alive >> (8*1)) & 0xff;
    data[9] = (msg.keep_alive >> (8*0)) & 0xff;

    char size = 9;
    size = addString(size, data, msg.client_id);

    if (msg.flags & WILL_FLAG) {
        size = addString(size, data, msg.will_topic);
        size = addString(size, data, msg.will_message);
    }

    if (msg.flags & USERNAME_FLAG) {
        size = addString(size, data, msg.username);
    }

    if (msg.flags & PASSWORD_FLAG) {
        size = addString(size, data, msg.password);
    }

    size++;

    char encoded_size[4];
    int encoded_size_len = encode(size, encoded_size);

    char buffer[1 + encoded_size_len + size] = { 0 };
    buffer[0] = 0b00010000;

    for (int i = 0; i < encoded_size_len; i++) {
        buffer[i+1] = encoded_size[i];
    }
    for (int i = 0; i < size+1; i++) {
        buffer[1+encoded_size_len+i] = data[i];
    }

    write(fd, buffer, sizeof(buffer));
}

void mqtt_publish(int fd, pub_message msg) {
    char data[2048];
    int size = addString(-1, data, msg.topic_name);
    if (msg.qos > 0) {
        data[size+1] = (msg.packet_id >> (8*1)) & 0xff;
        data[size+2] = (msg.packet_id >> (8*0)) & 0xff;
        size += 2;
    }

    for (int i = 0; i < strlen(msg.payload); i++) {
        size++;
        data[size] = msg.payload[i];
    }

    char encoded_size[4];
    int encoded_size_len = encode(size, encoded_size);

    char buffer[1 + encoded_size_len + size] = { 0 };

    buffer[0] = 0b00110000;
    if (msg.dup) buffer[0] = buffer[0] | 0b00001000;
    if (msg.qos == 1) buffer[0] = buffer[0] | 0b00000010;
    if (msg.qos == 2) buffer[0] = buffer[0] | 0b00000100;
    if (msg.retain) buffer[0] = buffer[0] | 0b00000001;

    for (int i = 0; i < encoded_size_len; i++) {
        buffer[i+1] = encoded_size[i];
    }
    for (int i = 0; i < size+1; i++) {
        buffer[1+encoded_size_len+i] = data[i];
    }

    write(fd,buffer,sizeof(buffer));
}


void mqtt_subscribe(int fd, sub_msg msg) {
    char data[2048];
    data[0] = (msg.packet_id >> (8*1)) & 0xff;
    data[1] = (msg.packet_id >> (8*0)) & 0xff;

    data[2] = (strlen(msg.topic_name) >> (8*1)) & 0xff;
    data[3] = (strlen(msg.topic_name) >> (8*0)) & 0xff;
    
    int size = 3;
    for (int i = 0; i < strlen(msg.topic_name); i++) {
        size++;
        data[size] = msg.topic_name[i];
    }
    size++;
    data[size] = msg.qos;
    size++;
    
    char encoded_size[4];
    int encoded_size_len = encode(size, encoded_size);

    char buffer[1 + encoded_size_len + size] = { 0 };

    buffer[0] = 0b10000010;
    for (int i = 0; i < encoded_size_len; i++) {
        buffer[i+1] = encoded_size[i];
    }
    for (int i = 0; i < size+1; i++) {
        buffer[1+encoded_size_len+i] = data[i];
    }

    write(fd,buffer,sizeof(buffer));
}